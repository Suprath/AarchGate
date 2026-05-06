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
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <arm_neon.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace apex {

ApexEngine::ApexEngine() noexcept {
    // Default to 4 (safe baseline)
    num_threads_ = 4;

#ifdef __APPLE__
    // Auto-detect Performance Cores (P-cores) on Apple Silicon
    int64_t perf_cores = 0;
    size_t size = sizeof(perf_cores);
    if (sysctlbyname("hw.perflevel0.logicalcpu", &perf_cores, &size, nullptr, 0) == 0) {
        num_threads_ = (int)perf_cores;
    }
#endif

    // Override with environment variable if present (e.g., APEX_THREADS=8)
    const char* env_threads = std::getenv("APEX_THREADS");
    if (env_threads) {
        int count = std::atoi(env_threads);
        if (count > 0) num_threads_ = count;
    }
}

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

void ApexEngine::set_expression(std::string_view schema_name, ir::Node* expr_root, ExecutionMode mode) {
    if (!expr_root) return;

    // STEP 1: Analyze active bits (only reads CONST values — safe before compilation)
    int active_bits = 0;
    std::function<void(const ir::Node*)> analyze_bits = [&](const ir::Node* n) {
        if (!n) return;
        if (n->kind == ir::NodeKind::CONST) {
            uint64_t val = static_cast<uint64_t>(n->const_value);
            int bits = (val == 0) ? 1 : 64 - __builtin_clzll(val);
            active_bits = std::max(active_bits, bits);
        }
        for (auto* op : n->operands) analyze_bits(op);
        if (n->left)  analyze_bits(n->left);
        if (n->right) analyze_bits(n->right);
    };
    analyze_bits(expr_root);
    // Always use full 64-bit comparison. The active_bits optimization (using fewer bit-planes)
    // is only safe when field VALUES are also bounded (e.g., RF model lookup indices),
    // not just when the threshold constant is small. Since we cannot know field value ranges
    // at compile time, default to 64-bit precision for correctness.
    // TODO: Add an explicit RF-mode flag to re-enable the narrowed optimization.
    active_bits = 64;

    // STEP 1.5: Mark SUM and SELECT nodes as skip_jit BEFORE compilation
    // so the compiler knows they are handled natively by the engine.
    ir::Node* effective_root = expr_root;
    if (effective_root->kind == ir::NodeKind::ADD && 
        effective_root->left->kind == ir::NodeKind::SUM && 
        effective_root->right->kind == ir::NodeKind::CONST) {
        effective_root->skip_jit = true;
        effective_root->right->skip_jit = true;
        effective_root = effective_root->left;
    }

    if (effective_root->kind == ir::NodeKind::SUM) {
        effective_root->skip_jit = true;
        for (auto* op : effective_root->operands) {
            if (op->kind == ir::NodeKind::SELECT) {
                op->skip_jit = true;
            }
        }
    }

    // STEP 2: Compile — ExpressionAnalyzer sets node->field_idx and node->slot_id
    jit::ExprKernelFunc kernel = nullptr;
    if (mode == ExecutionMode::BIT_SLICED) {
        kernel = compiler_.compile_expression(expr_root, registry_, schema_name, active_bits);
    } else {
        kernel = compiler_.compile_scalar_expression(expr_root, registry_, schema_name);
    }
    if (!kernel) return;

    // STEP 3: Collect field descriptors (node->field_idx is now valid)
    std::vector<const core::FieldDescriptor*> fields(32, nullptr);
    std::function<void(ir::Node*)> collect_fields = [&](ir::Node* node) {
        if (!node) return;
        if (node->kind == ir::NodeKind::LOAD) {
            int idx = node->field_idx;
            if (idx >= 0 && idx < 32) {
                const auto* field = registry_.get_field(schema_name, std::string_view(node->field_name));
                if (field) fields[idx] = field;
            }
        }
        collect_fields(node->left);
        collect_fields(node->right);
        collect_fields(node->cond);
        for (auto* op : node->operands) collect_fields(op);
    };
    collect_fields(expr_root);

    size_t max_idx = 0;
    for (int i = 0; i < 32; ++i) if (fields[i]) max_idx = i + 1;
    fields.resize(max_idx);

    // STEP 4: Extract SUM/SELECT metadata (node->slot_id is now valid)
    uint64_t base_sum = 0;
    std::vector<int64_t> delta_weights;
    std::vector<int> masks_to_popcount;
    ir::Node* sum_node = expr_root;
    if (sum_node->kind == ir::NodeKind::ADD && 
        sum_node->left->kind == ir::NodeKind::SUM && 
        sum_node->right->kind == ir::NodeKind::CONST) {
        base_sum = sum_node->right->const_value;
        sum_node = sum_node->left;
    }

    if (sum_node->kind == ir::NodeKind::SUM) {
        for (auto* op : sum_node->operands) {
            if (op->kind == ir::NodeKind::SELECT) {
                masks_to_popcount.push_back(op->cond->slot_id);
                if (op->left && op->left->kind == ir::NodeKind::CONST) {
                    delta_weights.push_back(static_cast<int64_t>(op->left->const_value));
                } else {
                    delta_weights.push_back(op->weight);
                }
            } else {
                masks_to_popcount.push_back(op->slot_id);
                delta_weights.push_back(op->weight);
            }
        }
    }
    // STEP 5: Store compiled logic
    ExprCompiledLogic compiled;
    compiled.kernel          = kernel;
    compiled.fields          = std::move(fields);
    compiled.mode            = mode;
    compiled.result_kind     = expr_root->result_kind;
    compiled.base_sum        = base_sum;
    compiled.delta_weights   = std::move(delta_weights);
    compiled.masks_to_popcount = std::move(masks_to_popcount);
    compiled.active_bits     = active_bits;

    expr_logic_[std::string(schema_name)] = std::move(compiled);
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
    
    // SIMD Gather optimization for contiguous 64-bit fields (e.g., NYC Taxi)
    bool use_simd_5 = (expr_logic.fields.size() >= 5 && 
                       expr_logic.fields[0] && expr_logic.fields[1] &&
                       expr_logic.fields[2] && expr_logic.fields[3] &&
                       expr_logic.fields[4] &&
                       expr_logic.fields[0]->offset == 0 &&
                       expr_logic.fields[1]->offset == 8 &&
                       expr_logic.fields[2]->offset == 16 &&
                       expr_logic.fields[3]->offset == 24 &&
                       expr_logic.fields[4]->offset == 32);

    if (use_simd_5) {
        const uint8_t* base = static_cast<const uint8_t*>(data_ptr);
        for (size_t r = 0; r < row_count; ++r) {
            const uint64_t* row_ptr = reinterpret_cast<const uint64_t*>(base + r * row_stride);
            uint64x2_t v01 = vld1q_u64(row_ptr);
            uint64x2_t v23 = vld1q_u64(row_ptr + 2);
            uint64_t v4 = row_ptr[4];
            
            field_buffers_[0].data[r] = vgetq_lane_u64(v01, 0);
            field_buffers_[1].data[r] = vgetq_lane_u64(v01, 1);
            field_buffers_[2].data[r] = vgetq_lane_u64(v23, 0);
            field_buffers_[3].data[r] = vgetq_lane_u64(v23, 1);
            field_buffers_[4].data[r] = v4;
        }
        for (int i = 0; i < 5; ++i) {
            if (expr_logic.mode == ExecutionMode::BIT_SLICED) {
                slicer_.slice_n(field_buffers_[i].data, 64, static_cast<uint64_t*>(field_buffers_[i].data), expr_logic.active_bits);
            }
            field_planes_array_[i] = field_buffers_[i].data;
        }
        for (size_t i = 5; i < expr_logic.fields.size() && i < 32; ++i) {
            if (expr_logic.fields[i]) {
                gather_field(data_ptr, expr_logic.fields[i], row_stride, row_count, field_buffers_[i]);
                if (expr_logic.mode == ExecutionMode::BIT_SLICED) {
                    slicer_.slice_n(field_buffers_[i].data, 64, static_cast<uint64_t*>(field_buffers_[i].data), expr_logic.active_bits);
                }
                field_planes_array_[i] = field_buffers_[i].data;
            } else {
                field_planes_array_[i] = nullptr;
            }
        }
    } else {
        for (size_t i = 0; i < expr_logic.fields.size() && i < 32; ++i) {
            if (expr_logic.fields[i]) {
                gather_field(data_ptr, expr_logic.fields[i], row_stride, row_count, field_buffers_[i]);
                if (expr_logic.mode == ExecutionMode::BIT_SLICED) {
                    slicer_.slice_n(field_buffers_[i].data, 64, static_cast<uint64_t*>(field_buffers_[i].data), expr_logic.active_bits);
                }
                field_planes_array_[i] = field_buffers_[i].data;
            } else {
                field_planes_array_[i] = nullptr;
            }
        }
    }
    // Call the JIT kernel via volatile pointer to prevent memoization
    auto (*volatile kernel_ptr)(const uint64_t* const*, uint64_t*) = expr_logic.kernel;
    if (kernel_ptr) {
        return kernel_ptr(field_planes_array_, scratchpad);
    }
    return 0;
}

uint64_t ApexEngine::execute(const void* data_ptr, size_t row_count) {
    // Check for expression-based logic first
    if (!expr_logic_.empty()) {
        auto meta_it = expr_logic_.begin();
        if (meta_it != expr_logic_.end()) {
            const std::string& schema_name = meta_it->first;
            const ExprCompiledLogic& expr_logic = meta_it->second;

            auto schema_it = schema_metadata_.find(schema_name);
            if (schema_it == schema_metadata_.end()) return 0;

            size_t row_stride = schema_it->second.row_stride;
            int64_t total_matches = 0;
            const uint8_t* base = static_cast<const uint8_t*>(data_ptr);
 
            // Use heap allocation for large scratchpads to avoid stack smashing
            uint64_t* scratchpad = (uint64_t*)std::malloc(32768 * sizeof(uint64_t));
            if (!scratchpad) return 0;
 
            // Process in 64-row chunks
            for (size_t chunk = 0; chunk * 64 < row_count; chunk++) {
                const void* chunk_ptr = base + chunk * 64 * row_stride;
                size_t rows_in_chunk = std::min(size_t(64), row_count - chunk * 64);
                
                std::memset(scratchpad, 0, 32768 * sizeof(uint64_t));
                
                uint64_t chunk_mask = process_chunk_expr(chunk_ptr, row_stride, rows_in_chunk, expr_logic, scratchpad);
 
                if (expr_logic.result_kind == ir::ResultKind::BITMASK) {
                    uint64_t rows_mask = (rows_in_chunk == 64) ? ~0ULL : (1ULL << rows_in_chunk) - 1;
                    total_matches += static_cast<int64_t>(__builtin_popcountll(chunk_mask & rows_mask));
                } else if (!expr_logic.delta_weights.empty()) {
                    // HYBRID POPCOUNT AGGREGATOR
                    uint64_t rows_mask = (rows_in_chunk == 64) ? ~0ULL : (1ULL << rows_in_chunk) - 1;
                    int64_t chunk_sum = static_cast<int64_t>(expr_logic.base_sum) * rows_in_chunk;
                    for (size_t i = 0; i < expr_logic.delta_weights.size(); ++i) {
                        int mask_slot = expr_logic.masks_to_popcount[i];
                        int64_t pop = __builtin_popcountll(scratchpad[mask_slot] & rows_mask);
                        chunk_sum += pop * expr_logic.delta_weights[i];
                    }
                    total_matches += chunk_sum;
                } else {
                    // BITPLANE (Legacy): Sum(popcount(Plane_i) * 2^i)
                    uint64_t rows_mask = (rows_in_chunk == 64) ? ~0ULL : (1ULL << rows_in_chunk) - 1;
                    for (int i = 0; i < 64; ++i) {
                        total_matches += (static_cast<int64_t>(__builtin_popcountll(scratchpad[i] & rows_mask))) << i;
                    }
                }
            }
            std::free(scratchpad);
            return static_cast<uint64_t>(total_matches);
        }
    }

    // Fall back to legacy single-field logic
    int active_bits = 64;

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
        slicer_.slice_n(field_buffers_[0].data, 64, field_buffers_[0].data, active_bits);
        
        uint64_t chunk_mask = logic.kernel(field_buffers_[0].data);
        total_matches += static_cast<uint64_t>(__builtin_popcountll(chunk_mask));
    }

    return total_matches;
}

uint64_t ApexEngine::execute_parallel(const void* data_ptr, size_t row_count, int num_threads) noexcept {
    if (num_threads <= 0) num_threads = num_threads_;
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
    config.result_kind = static_cast<int>(expr_logic.result_kind);
    config.base_sum = expr_logic.base_sum;
    config.delta_weights = expr_logic.delta_weights;
    config.masks_to_popcount = expr_logic.masks_to_popcount;
    config.active_bits = expr_logic.active_bits;

    return compute::ParallelRunner::run(data_ptr, row_count, config, num_threads);
}

uint64_t ApexEngine::execute_native(std::string_view schema_name, const uint64_t* bit_planes, size_t num_blocks) noexcept {
    auto it = expr_logic_.find(std::string(schema_name));
    if (it == expr_logic_.end()) return 0;
    const auto& logic = it->second;
    
    auto schema_it = schema_metadata_.find(std::string(schema_name));
    if (schema_it == schema_metadata_.end()) return 0;
    const auto& schema_fields = schema_it->second.fields;
    
    size_t num_fields = logic.fields.size();
    size_t schema_num_fields = schema_fields.size();
    std::vector<size_t> dynamic_to_schema_index(num_fields, 0);
    for (size_t i = 0; i < num_fields; ++i) {
        if (logic.fields[i]) {
            std::string_view name = logic.fields[i]->name;
            for (size_t f = 0; f < schema_fields.size(); ++f) {
                if (schema_fields[f].name == name) {
                    dynamic_to_schema_index[i] = f;
                    break;
                }
            }
        }
    }

    uint64_t total_matches = 0;
    
    uint64_t* scratchpad = nullptr;
    if (posix_memalign((void**)&scratchpad, 64, 262144 * sizeof(uint64_t)) != 0) return 0;
    std::memset(scratchpad, 0, 262144 * sizeof(uint64_t));
    
    for (size_t b = 0; b < num_blocks; ++b) {
        const uint64_t* block_base = bit_planes + (b * schema_num_fields * 64);
        
        const uint64_t* kernel_ptrs[8] = {nullptr};
        for (size_t i = 0; i < num_fields; ++i) {
            kernel_ptrs[i] = block_base + (dynamic_to_schema_index[i] * 64);
        }

        if (logic.result_kind == ir::ResultKind::BITMASK) {
            uint64_t mask = logic.kernel(kernel_ptrs, scratchpad);
            total_matches += static_cast<uint64_t>(__builtin_popcountll(mask));
        } else if (!logic.delta_weights.empty()) {
            logic.kernel(kernel_ptrs, scratchpad);
            
            // SIMD Aggregation: Process 16 masks at once
            // This is the key to 100M+ RPS
            // SIMD Aggregation: Sum(popcount_i * weight_i)
            int64_t chunk_sum = static_cast<int64_t>(logic.base_sum) * 64;
            size_t i = 0;
            const size_t num_trees = logic.delta_weights.size();
            
            // Unroll 16x for maximum ILP
            for (; i + 16 <= num_trees; i += 16) {
                // We use NEON CNT and UADDLV in a tight block
                // For now, use the unrolled scalar loop as a placeholder that the compiler can vectorize
                for (int j = 0; j < 16; ++j) {
                    chunk_sum += __builtin_popcountll(scratchpad[logic.masks_to_popcount[i+j]]) * logic.delta_weights[i+j];
                }
            }
            for (; i < num_trees; ++i) {
                chunk_sum += __builtin_popcountll(scratchpad[logic.masks_to_popcount[i]]) * logic.delta_weights[i];
            }
            total_matches += static_cast<uint64_t>(chunk_sum);
        } else {
            logic.kernel(kernel_ptrs, scratchpad);
            for (int i = 0; i < 64; ++i) {
                total_matches += (static_cast<uint64_t>(__builtin_popcountll(scratchpad[i]))) << i;
            }
        }
    }
    
    free(scratchpad);
    return total_matches;
}

uint64_t ApexEngine::execute_native_parallel(std::string_view schema_name, const uint64_t* bit_planes, size_t num_blocks, int num_threads) noexcept {
    if (num_threads <= 0) num_threads = num_threads_;
    auto it = expr_logic_.find(std::string(schema_name));
    if (it == expr_logic_.end()) return 0;
    const auto& logic = it->second;
    
    auto schema_it = schema_metadata_.find(std::string(schema_name));
    if (schema_it == schema_metadata_.end()) return 0;
    const auto& schema_fields = schema_it->second.fields;
    
    size_t num_fields = logic.fields.size();
    size_t schema_num_fields = schema_fields.size();
    std::vector<size_t> dynamic_to_schema_index(num_fields, 0);
    for (size_t i = 0; i < num_fields; ++i) {
        if (logic.fields[i]) {
            std::string_view name = logic.fields[i]->name;
            for (size_t f = 0; f < schema_fields.size(); ++f) {
                if (schema_fields[f].name == name) {
                    dynamic_to_schema_index[i] = f;
                    break;
                }
            }
        }
    }

    std::vector<std::thread> threads;
    std::vector<uint64_t> results(num_threads, 0);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            size_t start_block = (num_blocks / num_threads) * t;
            size_t end_block = (t == num_threads - 1) ? num_blocks : start_block + (num_blocks / num_threads);
            
            uint64_t* scratchpad = nullptr;
            if (posix_memalign((void**)&scratchpad, 64, 262144 * sizeof(uint64_t)) != 0) return;
            std::memset(scratchpad, 0, 262144 * sizeof(uint64_t));
            
            for (size_t b = start_block; b < end_block; ++b) {
                const uint64_t* block_base = bit_planes + (b * schema_num_fields * 64);

                const uint64_t* kernel_ptrs[8] = {nullptr};
                for (size_t i = 0; i < num_fields; ++i) {
                    kernel_ptrs[i] = block_base + (dynamic_to_schema_index[i] * 64);
                }

                if (logic.result_kind == ir::ResultKind::BITMASK) {
                    uint64_t mask = logic.kernel(kernel_ptrs, scratchpad);
                    results[t] += static_cast<uint64_t>(__builtin_popcountll(mask));
                } else if (!logic.delta_weights.empty()) {
                    logic.kernel(kernel_ptrs, scratchpad);
                    int64_t chunk_sum = static_cast<int64_t>(logic.base_sum) * 64;
                    size_t i = 0;
                    for (; i + 16 <= logic.delta_weights.size(); i += 16) {
                        for(int j=0; j<16; ++j) {
                            chunk_sum += __builtin_popcountll(scratchpad[logic.masks_to_popcount[i+j]]) * logic.delta_weights[i+j];
                        }
                    }
                    for (; i < logic.delta_weights.size(); ++i) {
                        chunk_sum += __builtin_popcountll(scratchpad[logic.masks_to_popcount[i]]) * logic.delta_weights[i];
                    }
                    results[t] += static_cast<uint64_t>(chunk_sum);
                } else {
                    logic.kernel(kernel_ptrs, scratchpad);
                    for (int i = 0; i < 64; ++i) {
                        results[t] += (static_cast<uint64_t>(__builtin_popcountll(scratchpad[i]))) << i;
                    }
                }
            }
            free(scratchpad);
        });
    }

    uint64_t total = 0;
    for (auto& th : threads) th.join();
    for (uint64_t r : results) total += r;
    return total;
}

} // namespace apex
