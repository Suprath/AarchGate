// (c) 2024-2026 Suprath PS. All rights reserved.
// Project Apex: Universal JIT-Accelerated Vector Engine (10B+ RPS)
//
// This work is licensed under the Business Source License 1.1 until 2029-05-03,
// transitioning to the Apache License 2.0 thereafter.
// See the LICENSE file in the repository root for the full text.

#include "apex/engine.hpp"
#include "apex/compute/parallel_runner.hpp"
#include <algorithm>
#include <iostream>
#include <cstring>
#include <vector>
#include <functional>

namespace apex {

ApexEngine::ApexEngine() noexcept = default;

void ApexEngine::register_schema(std::string_view schema_name,
                               const std::vector<core::FieldDescriptor>& fields,
                               size_t total_row_stride) noexcept {
    registry_.register_schema(schema_name, fields);
    schema_metadata_[std::string(schema_name)] = {total_row_stride, fields};
}

void ApexEngine::set_logic(std::string_view schema_name,
                          std::string_view field_name,
                          uint64_t threshold) noexcept {
    const auto* field = registry_.get_field(schema_name, field_name);
    if (!field) return;

    jit::KernelFunc kernel = compiler_.compile_comparison(threshold);
    compiled_logic_[std::string(schema_name)] = {field, kernel};
}

void ApexEngine::set_expression(std::string_view schema_name, ir::Node* expr_root, ExecutionMode mode) noexcept {
    jit::ExprKernelFunc kernel = nullptr;
    if (mode == ExecutionMode::BIT_SLICED) {
        kernel = compiler_.compile_expression(expr_root, registry_, schema_name);
    } else {
        kernel = compiler_.compile_scalar_expression(expr_root, registry_, schema_name);
    }
    if (!kernel) return;

    // Collect referenced fields from the expression tree into their JIT-assigned slots
    std::vector<const core::FieldDescriptor*> fields(8, nullptr); 

    std::function<void(ir::Node*)> collect_fields = [&](ir::Node* node) {
        if (!node) return;
        if (node->kind == ir::NodeKind::LOAD) {
            int idx = node->field_idx;
            if (idx >= 0 && idx < 8) {
                const auto* field = registry_.get_field(schema_name, std::string_view(node->field_name));
                if (field) {
                    fields[idx] = field;
                }
            }
        }
        collect_fields(node->left);
        collect_fields(node->right);
        if (node->kind == ir::NodeKind::SELECT) {
            collect_fields(node->cond);
        }
    };

    collect_fields(expr_root);

    // Trim the fields vector to remove trailing nulls, but keep internal nulls if any field was missing
    size_t max_idx = 0;
    for (int i = 0; i < 8; ++i) {
        if (fields[i]) max_idx = i + 1;
    }
    fields.resize(max_idx);

    std::string expr_key = std::string(schema_name);
    expr_logic_[expr_key] = {kernel, fields, mode};
}

void ApexEngine::gather_field(const void* data_ptr,
                             const core::FieldDescriptor* field,
                             size_t row_stride,
                             size_t row_count,
                             compute::ColumnBuffer& out) const noexcept {
    const uint8_t* base = static_cast<const uint8_t*>(data_ptr);
    const size_t offset = field->offset;
    const size_t rows_to_gather = std::min(row_count, size_t(64));

    // Unrolled gather loop for high-performance bit-slicing
    size_t i = 0;
    for (; i + 4 <= rows_to_gather; i += 4) {
        uint64_t v0, v1, v2, v3;
        std::memcpy(&v0, base + (i + 0) * row_stride + offset, 8);
        std::memcpy(&v1, base + (i + 1) * row_stride + offset, 8);
        std::memcpy(&v2, base + (i + 2) * row_stride + offset, 8);
        std::memcpy(&v3, base + (i + 3) * row_stride + offset, 8);
        out.data[i + 0] = v0;
        out.data[i + 1] = v1;
        out.data[i + 2] = v2;
        out.data[i + 3] = v3;
    }
    for (; i < rows_to_gather; ++i) {
        uint64_t v0;
        std::memcpy(&v0, base + i * row_stride + offset, 8);
        out.data[i] = v0;
    }
    // Zero-pad remaining
    for (; i < 64; ++i) {
        out.data[i] = 0;
    }
}

uint64_t ApexEngine::process_chunk_expr(const void* data_ptr,
                                     size_t row_stride,
                                     size_t row_count,
                                     const ExprCompiledLogic& expr_logic,
                                     uint64_t* scratchpad) noexcept {
    // Reset field planes array
    std::memset(field_planes_array_, 0, sizeof(field_planes_array_));
    
    for (size_t i = 0; i < expr_logic.fields.size() && i < 8; ++i) {
        if (expr_logic.fields[i]) {
            gather_field(data_ptr, expr_logic.fields[i], row_stride, row_count, field_buffers_[i]);
            if (expr_logic.mode == ExecutionMode::BIT_SLICED) {
                slicer_.slice(field_buffers_[i], field_buffers_[i]);
            }
            field_planes_array_[i] = field_buffers_[i].data;
        } else {
            field_planes_array_[i] = nullptr;
        }
    }
    // Call the JIT kernel
    if (expr_logic.kernel) {
        return expr_logic.kernel(field_planes_array_, scratchpad);
    }
    return 0;
}

uint64_t ApexEngine::execute(const void* data_ptr, size_t row_count) noexcept {
    // Check for expression-based logic first
    if (!expr_logic_.empty()) {
        auto meta_it = expr_logic_.begin();
        if (meta_it != expr_logic_.end()) {
            const std::string& schema_name = meta_it->first;
            const ExprCompiledLogic& expr_logic = meta_it->second;

            auto schema_it = schema_metadata_.find(schema_name);
            if (schema_it == schema_metadata_.end()) return 0;

            size_t row_stride = schema_it->second.row_stride;
            uint64_t total_matches = 0;
            const uint8_t* base = static_cast<const uint8_t*>(data_ptr);

            // Pre-allocate aligned scratchpad once per execution
            uint64_t* scratchpad = nullptr;
            if (posix_memalign((void**)&scratchpad, 64, 8 * 64 * sizeof(uint64_t)) != 0) {
                return 0;
            }

            // Process in 64-row chunks
            for (size_t chunk = 0; chunk * 64 < row_count; chunk++) {
                const void* chunk_ptr = base + chunk * 64 * row_stride;
                size_t rows_in_chunk = std::min(size_t(64), row_count - chunk * 64);

                std::memset(scratchpad, 0, 8 * 64 * sizeof(uint64_t));
                uint64_t chunk_mask = process_chunk_expr(chunk_ptr, row_stride, rows_in_chunk, expr_logic, scratchpad);
                total_matches += static_cast<uint64_t>(__builtin_popcountll(chunk_mask));
            }

            free(scratchpad);
            return total_matches;
        }
    }

    // Fall back to legacy single-field logic
    if (compiled_logic_.empty()) return 0;

    const auto& [logic_key, logic] = *compiled_logic_.begin();
    auto schema_it = schema_metadata_.find(logic_key);
    if (schema_it == schema_metadata_.end()) return 0;

    size_t row_stride = schema_it->second.row_stride;
    uint64_t total_matches = 0;
    const uint8_t* base = static_cast<const uint8_t*>(data_ptr);

    for (size_t chunk = 0; chunk * 64 < row_count; chunk++) {
        const void* chunk_ptr = base + chunk * 64 * row_stride;
        size_t rows_in_chunk = std::min(size_t(64), row_count - chunk * 64);

        gather_field(chunk_ptr, logic.field, row_stride, rows_in_chunk, field_buffers_[0]);
        slicer_.slice(field_buffers_[0], field_buffers_[0]);
        
        uint64_t chunk_mask = logic.kernel(field_buffers_[0].data);
        total_matches += static_cast<uint64_t>(__builtin_popcountll(chunk_mask));
    }

    return total_matches;
}

uint64_t ApexEngine::execute_parallel(const void* data_ptr, size_t row_count, int num_threads) noexcept {
    if (expr_logic_.empty()) return execute(data_ptr, row_count);

    auto meta_it = expr_logic_.begin();
    const std::string& schema_name = meta_it->first;
    const ExprCompiledLogic& expr_logic = meta_it->second;

    auto schema_it = schema_metadata_.find(schema_name);
    if (schema_it == schema_metadata_.end()) return 0;

    compute::ParallelRunner::TaskConfig config;
    config.kernel = expr_logic.kernel;
    config.fields = expr_logic.fields;
    config.row_stride = schema_it->second.row_stride;
    config.mode = expr_logic.mode;

    return compute::ParallelRunner::run(data_ptr, row_count, config, num_threads);
}

} // namespace apex
