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
#include <unordered_set>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <arm_neon.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include "apex/gpu/metal_device.hpp"
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
    // so the compiler knows they are handled natively by the engine on CPU.
    // For GPU execution, we need to compile the entire AST tree, so we reset skip_jit to false.
    if (mode == ExecutionMode::BIT_SLICED) {
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
    } else if (mode == ExecutionMode::GPU_THROUGHPUT) {
        std::function<void(ir::Node*)> reset_skip_jit = [&](ir::Node* n) {
            if (!n) return;
            n->skip_jit = false;
            reset_skip_jit(n->left);
            reset_skip_jit(n->right);
            reset_skip_jit(n->cond);
            for (auto* op : n->operands) reset_skip_jit(op);
        };
        reset_skip_jit(expr_root);
    }

    // STEP 2: Compile — ExpressionAnalyzer sets node->field_idx and node->slot_id
    jit::ExprKernelFunc kernel = nullptr;
    if (mode == ExecutionMode::BIT_SLICED || mode == ExecutionMode::GPU_THROUGHPUT) {
        kernel = compiler_.compile_expression(expr_root, registry_, schema_name, active_bits);
    } else {
        kernel = compiler_.compile_scalar_expression(expr_root, registry_, schema_name);
    }
    if (!kernel) return;

    // STEP 3: Collect field descriptors (node->field_idx is now valid)
    std::vector<const core::FieldDescriptor*> fields(128, nullptr);
    std::function<void(ir::Node*)> collect_fields = [&](ir::Node* node) {
        if (!node) return;
        if (node->kind == ir::NodeKind::LOAD) {
            int idx = node->field_idx;
            if (idx >= 0 && idx < 128) {
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
    for (int i = 0; i < 128; ++i) if (fields[i]) max_idx = i + 1;
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
                int64_t right_val = 0;
                if (op->right && op->right->kind == ir::NodeKind::CONST) {
                    right_val = static_cast<int64_t>(op->right->const_value);
                }
                base_sum += right_val;
                if (op->left && op->left->kind == ir::NodeKind::CONST) {
                    delta_weights.push_back(static_cast<int64_t>(op->left->const_value) - right_val);
                } else {
                    delta_weights.push_back(static_cast<int64_t>(op->weight) - right_val);
                }
            } else {
                masks_to_popcount.push_back(op->slot_id);
                delta_weights.push_back(op->weight);
            }
        }
    }
    std::string msl_source;
    if (mode == ExecutionMode::GPU_THROUGHPUT) {
        msl_source = generate_msl_source(expr_root, schema_name);
    }

    // STEP 5: Store compiled logic
    ExprCompiledLogic compiled;
    compiled.kernel          = kernel;
    compiled.fields          = std::move(fields);
    compiled.mode            = mode;
    compiled.result_kind     = expr_root->result_kind;
    compiled.msl_source      = std::move(msl_source);
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
        for (size_t i = 5; i < expr_logic.fields.size() && i < 128; ++i) {
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
        for (size_t i = 0; i < expr_logic.fields.size() && i < 128; ++i) {
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

            if (expr_logic.mode == ExecutionMode::GPU_THROUGHPUT) {
                #ifdef __APPLE__
                size_t num_blocks = (row_count + 63) / 64;
                size_t num_fields = expr_logic.fields.size();
                size_t row_stride = schema_it->second.row_stride;
                const uint8_t* base = static_cast<const uint8_t*>(data_ptr);

                uint64_t* bit_planes = nullptr;
                size_t size = num_blocks * num_fields * 64 * sizeof(uint64_t);
                if (posix_memalign((void**)&bit_planes, 4096, size) == 0) {
                    std::memset(bit_planes, 0, size);

                    size_t num_workers = 4; // Limit to 4 to run exclusively on M3 Performance Cores and avoid E-core stragglers
                    if (num_blocks < num_workers) num_workers = num_blocks;

                    std::vector<std::thread> workers;
                    workers.reserve(num_workers);

                    for (size_t w = 0; w < num_workers; ++w) {
                        size_t start_block = (num_blocks * w) / num_workers;
                        size_t end_block = (num_blocks * (w + 1)) / num_workers;

                        workers.emplace_back([=, &expr_logic]() {
                            compute::BitSlicer slicer;
                            for (size_t block = start_block; block < end_block; ++block) {
                                size_t rows_in_block = std::min(size_t(64), row_count - block * 64);
                                const void* block_ptr = base + block * 64 * row_stride;
                                
                                for (size_t f = 0; f < num_fields; ++f) {
                                    if (expr_logic.fields[f]) {
                                        compute::ColumnBuffer temp_buf;
                                        gather_field(block_ptr, expr_logic.fields[f], row_stride, rows_in_block, temp_buf);
                                        
                                        uint64_t* block_dest = bit_planes + (block * num_fields * 64) + (f * 64);
                                        slicer.slice_n(temp_buf.data, rows_in_block, block_dest, 64);
                                    }
                                }
                            }
                        });
                    }

                    for (auto& worker : workers) {
                        worker.join();
                    }

                    auto& device = gpu::MetalDevice::instance();
                    uint64_t final_sum = device.dispatch(schema_name, bit_planes, num_blocks, num_fields, expr_logic.msl_source, expr_logic.delta_weights, expr_logic.base_sum);
                    std::free(bit_planes);
                    return final_sum;
                }
                #endif
                return 0;
            }
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

    if (logic.mode == ExecutionMode::GPU_THROUGHPUT) {
        #ifdef __APPLE__
        auto& device = gpu::MetalDevice::instance();
        return device.dispatch(schema_name, bit_planes, num_blocks, logic.fields.size(), logic.msl_source, logic.delta_weights, logic.base_sum);
        #else
        std::cerr << "[AARCHGATE ERROR] Metal GPU is only supported on Apple macOS!" << std::endl;
        return 0;
        #endif
    }
    
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

std::vector<double> ApexEngine::execute_vector(const void* data_ptr, size_t row_count, double precision_multiplier) {
    std::vector<double> results(row_count, 0.0);
    if (expr_logic_.empty()) return results;

    auto meta_it = expr_logic_.begin();
    if (meta_it == expr_logic_.end()) return results;

    const std::string& schema_name = meta_it->first;
    const ExprCompiledLogic& expr_logic = meta_it->second;

    auto schema_it = schema_metadata_.find(schema_name);
    if (schema_it == schema_metadata_.end()) return results;

    size_t row_stride = schema_it->second.row_stride;
    const uint8_t* base = static_cast<const uint8_t*>(data_ptr);

    uint64_t* scratchpad = (uint64_t*)std::malloc(32768 * sizeof(uint64_t));
    if (!scratchpad) return results;

    for (size_t chunk = 0; chunk * 64 < row_count; chunk++) {
        const void* chunk_ptr = base + chunk * 64 * row_stride;
        size_t rows_in_chunk = std::min(size_t(64), row_count - chunk * 64);
        
        std::memset(scratchpad, 0, 32768 * sizeof(uint64_t));
        
        uint64_t chunk_mask = process_chunk_expr(chunk_ptr, row_stride, rows_in_chunk, expr_logic, scratchpad);

        size_t start_idx = chunk * 64;
        if (expr_logic.result_kind == ir::ResultKind::BITMASK) {
            for (size_t r = 0; r < rows_in_chunk; ++r) {
                double val = ((chunk_mask >> r) & 1) ? 1.0 : 0.0;
                results[start_idx + r] = val;
            }
        } else if (!expr_logic.delta_weights.empty()) {
            for (size_t r = 0; r < rows_in_chunk; ++r) {
                int64_t row_sum = static_cast<int64_t>(expr_logic.base_sum);
                for (size_t i = 0; i < expr_logic.delta_weights.size(); ++i) {
                    int mask_slot = expr_logic.masks_to_popcount[i];
                    if ((scratchpad[mask_slot] >> r) & 1) {
                        row_sum += expr_logic.delta_weights[i];
                    }
                }
                results[start_idx + r] = static_cast<double>(row_sum) / precision_multiplier;
            }
        } else {
            for (size_t r = 0; r < rows_in_chunk; ++r) {
                int64_t row_sum = 0;
                for (int b = 0; b < 64; ++b) {
                    if ((scratchpad[b] >> r) & 1) {
                        row_sum |= (1ULL << b);
                    }
                }
                results[start_idx + r] = static_cast<double>(row_sum) / precision_multiplier;
            }
        }
    }
    std::free(scratchpad);
    return results;
}

std::string ApexEngine::generate_msl_source(ir::Node* root, std::string_view schema_name) const {
    size_t num_fields = 0;
    auto schema_it = schema_metadata_.find(std::string(schema_name));
    if (schema_it != schema_metadata_.end()) {
        num_fields = schema_it->second.fields.size();
    }

    // Dynamic active bits analysis to narrow GPU loop boundaries
    int bits_needed = 0;
    std::function<void(const ir::Node*)> analyze_bits = [&](const ir::Node* n) {
        if (!n) return;
        if (n->kind == ir::NodeKind::CONST) {
            uint64_t val = static_cast<uint64_t>(n->const_value);
            int bits = (val == 0) ? 1 : 64 - __builtin_clzll(val);
            bits_needed = std::max(bits_needed, bits);
        }
        for (auto* op : n->operands) analyze_bits(op);
        if (n->left)  analyze_bits(n->left);
        if (n->right) analyze_bits(n->right);
    };
    analyze_bits(root);
    int active_bits = 64;

    // 1. Perform Topological Sorting (Post-Order DFS) first to determine maximum scratchpad slot usage
    std::vector<ir::Node*> post_order;
    std::unordered_set<ir::Node*> visited;
    std::function<void(ir::Node*)> dfs = [&](ir::Node* n) {
        if (!n || visited.count(n)) return;
        visited.insert(n);
        dfs(n->left);
        dfs(n->right);
        dfs(n->cond);
        for (auto* op : n->operands) dfs(op);
        post_order.push_back(n);
    };
    dfs(root);

    // Assign a dedicated, sequential slot_id to every node in the tree for GPU scratchpad isolation
    size_t next_slot_id = 0;
    for (auto* node : post_order) {
        if (!node->skip_jit) {
            node->slot_id = next_slot_id++;
        }
    }
    size_t scratchpad_size = next_slot_id;
    if (scratchpad_size == 0) scratchpad_size = 1;

    // 2. Core Kogge-Stone parallel prefix scan helpers and evaluate kernel header
    std::string msl = 
        "#include <metal_stdlib>\n"
        "using namespace metal;\n\n"
        "inline uint64_t shuffle_up_64(uint64_t val, ushort delta) {\n"
        "    uint2 parts = as_type<uint2>(val);\n"
        "    parts.x = simd_shuffle_up(parts.x, delta);\n"
        "    parts.y = simd_shuffle_up(parts.y, delta);\n"
        "    return as_type<uint64_t>(parts);\n"
        "}\n\n"
        "inline uint64_t shuffle_down_64(uint64_t val, ushort delta) {\n"
        "    uint2 parts = as_type<uint2>(val);\n"
        "    parts.x = simd_shuffle_down(parts.x, delta);\n"
        "    parts.y = simd_shuffle_down(parts.y, delta);\n"
        "    return as_type<uint64_t>(parts);\n"
        "}\n\n"
        "inline void kogge_stone_scan(uint64_t g, uint64_t p, thread uint64_t& cur_g, thread uint64_t& cur_p, ushort lane_id) {\n"
        "    cur_g = g;\n"
        "    cur_p = p;\n"
        "    uint64_t g_prev, p_prev;\n"
        "    g_prev = shuffle_up_64(cur_g, 1);\n"
        "    p_prev = shuffle_up_64(cur_p, 1);\n"
        "    if (lane_id >= 1) {\n"
        "        cur_g = cur_g | (cur_p & g_prev);\n"
        "        cur_p = cur_p & p_prev;\n"
        "    }\n"
        "    g_prev = shuffle_up_64(cur_g, 2);\n"
        "    p_prev = shuffle_up_64(cur_p, 2);\n"
        "    if (lane_id >= 2) {\n"
        "        cur_g = cur_g | (cur_p & g_prev);\n"
        "        cur_p = cur_p & p_prev;\n"
        "    }\n"
        "    g_prev = shuffle_up_64(cur_g, 4);\n"
        "    p_prev = shuffle_up_64(cur_p, 4);\n"
        "    if (lane_id >= 4) {\n"
        "        cur_g = cur_g | (cur_p & g_prev);\n"
        "        cur_p = cur_p & p_prev;\n"
        "    }\n"
        "    g_prev = shuffle_up_64(cur_g, 8);\n"
        "    p_prev = shuffle_up_64(cur_p, 8);\n"
        "    if (lane_id >= 8) {\n"
        "        cur_g = cur_g | (cur_p & g_prev);\n"
        "        cur_p = cur_p & p_prev;\n"
        "    }\n"
        "    g_prev = shuffle_up_64(cur_g, 16);\n"
        "    p_prev = shuffle_up_64(cur_p, 16);\n"
        "    if (lane_id >= 16) {\n"
        "        cur_g = cur_g | (cur_p & g_prev);\n"
        "        cur_p = cur_p & p_prev;\n"
        "    }\n"
        "}\n\n"
        "kernel void bit_sliced_evaluate(\n"
        "    device const uint64_t* columns         [[buffer(0)]],\n"
        "    device uint64_t* output_masks          [[buffer(1)]],\n"
        "    uint3 group_idx                        [[threadgroup_position_in_grid]],\n"
        "    uint thread_in_group                   [[thread_index_in_threadgroup]],\n"
        "    uint simd_id                           [[simdgroup_index_in_threadgroup]],\n"
        "    uint lane_id                           [[thread_index_in_simdgroup]]\n"
        ") {\n"
        "    const uint block_idx = group_idx.x;\n"
        "    const uint bit_idx = thread_in_group;\n\n"
        "    uint64_t scratchpad[" + std::to_string(scratchpad_size) + "] = {0};\n"
        "    threadgroup uint64_t shared_A[64];\n"
        "    threadgroup uint64_t shared_B[64];\n"
        "    threadgroup uint64_t shared_sums[2];\n"
        "    threadgroup uint64_t shared_carry;\n\n";

    // 3. Emit dynamic MSL operations for each analytical node
    for (auto* node : post_order) {
        if (node->skip_jit) continue;

        switch (node->kind) {
            case ir::NodeKind::LOAD: {
                msl += "    scratchpad[" + std::to_string(node->slot_id) + "] = columns[block_idx * " 
                       + std::to_string(num_fields) + " * 64 + " + std::to_string(node->field_idx) + " * 64 + bit_idx];\n";
                break;
            }
            case ir::NodeKind::CONST: {
                msl += "    scratchpad[" + std::to_string(node->slot_id) + "] = (((" 
                       + std::to_string(node->const_value) + "ULL) >> bit_idx) & 1) ? 0xFFFFFFFFFFFFFFFFULL : 0ULL;\n";
                break;
            }
            case ir::NodeKind::AND: {
                msl += "    scratchpad[" + std::to_string(node->slot_id) + "] = scratchpad[" 
                       + std::to_string(node->left->slot_id) + "] & scratchpad[" + std::to_string(node->right->slot_id) + "];\n";
                break;
            }
            case ir::NodeKind::OR: {
                msl += "    scratchpad[" + std::to_string(node->slot_id) + "] = scratchpad[" 
                       + std::to_string(node->left->slot_id) + "] | scratchpad[" + std::to_string(node->right->slot_id) + "];\n";
                break;
            }

            case ir::NodeKind::NOT: {
                msl += "    scratchpad[" + std::to_string(node->slot_id) + "] = ~scratchpad[" 
                       + std::to_string(node->left->slot_id) + "];\n";
                break;
            }
            case ir::NodeKind::SELECT: {
                msl += "    scratchpad[" + std::to_string(node->slot_id) + "] = (scratchpad[" 
                       + std::to_string(node->cond->slot_id) + "] & scratchpad[" + std::to_string(node->left->slot_id) + "]) | (~scratchpad[" 
                       + std::to_string(node->cond->slot_id) + "] & scratchpad[" + std::to_string(node->right->slot_id) + "]);\n";
                break;
            }
            case ir::NodeKind::GT:
            case ir::NodeKind::GE:
            case ir::NodeKind::LT:
            case ir::NodeKind::LE:
            case ir::NodeKind::EQ: {
                msl += "    {\n"
                       "        shared_A[bit_idx] = scratchpad[" + std::to_string(node->left->slot_id) + "];\n"
                       "        shared_B[bit_idx] = scratchpad[" + std::to_string(node->right->slot_id) + "];\n"
                       "        threadgroup_barrier(mem_flags::mem_threadgroup);\n";
                
                if (node->kind == ir::NodeKind::GT || node->kind == ir::NodeKind::GE) {
                    msl += "        uint64_t mask_gt = 0;\n"
                           "        uint64_t mask_eq = ~0ULL;\n"
                           "        for (int b = " + std::to_string(active_bits - 1) + "; b >= 0; --b) {\n"
                           "            uint64_t a_plane = shared_A[b];\n"
                           "            uint64_t b_plane = shared_B[b];\n"
                           "            mask_gt = mask_gt | (mask_eq & a_plane & ~b_plane);\n"
                           "            mask_eq = mask_eq & ~(a_plane ^ b_plane);\n"
                           "        }\n";
                    if (node->kind == ir::NodeKind::GT) {
                        msl += "        scratchpad[" + std::to_string(node->slot_id) + "] = mask_gt;\n";
                    } else {
                        msl += "        scratchpad[" + std::to_string(node->slot_id) + "] = mask_gt | mask_eq;\n";
                    }
                } else if (node->kind == ir::NodeKind::LT || node->kind == ir::NodeKind::LE) {
                    msl += "        uint64_t mask_lt = 0;\n"
                           "        uint64_t mask_eq = ~0ULL;\n"
                           "        for (int b = " + std::to_string(active_bits - 1) + "; b >= 0; --b) {\n"
                           "            uint64_t a_plane = shared_A[b];\n"
                           "            uint64_t b_plane = shared_B[b];\n"
                           "            mask_lt = mask_lt | (mask_eq & ~a_plane & b_plane);\n"
                           "            mask_eq = mask_eq & ~(a_plane ^ b_plane);\n"
                           "        }\n";
                    if (node->kind == ir::NodeKind::LT) {
                        msl += "        scratchpad[" + std::to_string(node->slot_id) + "] = mask_lt;\n";
                    } else {
                        msl += "        scratchpad[" + std::to_string(node->slot_id) + "] = mask_lt | mask_eq;\n";
                    }
                } else if (node->kind == ir::NodeKind::EQ) {
                    msl += "        uint64_t mask_eq = ~0ULL;\n"
                           "        for (int b = " + std::to_string(active_bits - 1) + "; b >= 0; --b) {\n"
                           "            uint64_t a_plane = shared_A[b];\n"
                           "            uint64_t b_plane = shared_B[b];\n"
                           "            mask_eq = mask_eq & ~(a_plane ^ b_plane);\n"
                           "        }\n"
                           "        scratchpad[" + std::to_string(node->slot_id) + "] = mask_eq;\n";
                }
                msl += "        threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                       "    }\n";
                break;
            }
            case ir::NodeKind::ADD: {
                msl += "    {\n"
                       "        uint64_t g = scratchpad[" + std::to_string(node->left->slot_id) + "] & scratchpad[" + std::to_string(node->right->slot_id) + "];\n"
                       "        uint64_t p = scratchpad[" + std::to_string(node->left->slot_id) + "] ^ scratchpad[" + std::to_string(node->right->slot_id) + "];\n"
                       "        uint64_t cur_g, cur_p;\n"
                       "        kogge_stone_scan(g, p, cur_g, cur_p, lane_id);\n"
                       "        if (lane_id == 31 && simd_id == 0) {\n"
                       "            shared_carry = cur_g;\n"
                       "        }\n"
                       "        threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                       "        if (simd_id == 1) {\n"
                       "            cur_g = cur_g | (shared_carry & cur_p);\n"
                       "        }\n"
                       "        uint64_t carry_in = 0;\n"
                       "        if (simd_id == 0) {\n"
                       "            carry_in = (lane_id == 0) ? 0ULL : shuffle_up_64(cur_g, 1);\n"
                       "        } else {\n"
                       "            carry_in = (lane_id == 0) ? shared_carry : shuffle_up_64(cur_g, 1);\n"
                       "        }\n"
                       "        scratchpad[" + std::to_string(node->slot_id) + "] = scratchpad[" + std::to_string(node->left->slot_id) + "] ^ scratchpad[" + std::to_string(node->right->slot_id) + "] ^ carry_in;\n"
                       "    }\n";
                break;
            }
            case ir::NodeKind::SUB: {
                msl += "    {\n"
                       "        uint64_t g = scratchpad[" + std::to_string(node->left->slot_id) + "] & ~scratchpad[" + std::to_string(node->right->slot_id) + "];\n"
                       "        uint64_t p = scratchpad[" + std::to_string(node->left->slot_id) + "] ^ ~scratchpad[" + std::to_string(node->right->slot_id) + "];\n"
                       "        if (lane_id == 0 && simd_id == 0) {\n"
                       "            g = g | p;\n"
                       "        }\n"
                       "        uint64_t cur_g, cur_p;\n"
                       "        kogge_stone_scan(g, p, cur_g, cur_p, lane_id);\n"
                       "        if (lane_id == 31 && simd_id == 0) {\n"
                       "            shared_carry = cur_g;\n"
                       "        }\n"
                       "        threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                       "        if (simd_id == 1) {\n"
                       "            cur_g = cur_g | (shared_carry & cur_p);\n"
                       "        }\n"
                       "        uint64_t carry_in = 0;\n"
                       "        if (simd_id == 0) {\n"
                       "            carry_in = (lane_id == 0) ? 1ULL : shuffle_up_64(cur_g, 1);\n"
                       "        } else {\n"
                       "            carry_in = (lane_id == 0) ? shared_carry : shuffle_up_64(cur_g, 1);\n"
                       "        }\n"
                       "        scratchpad[" + std::to_string(node->slot_id) + "] = scratchpad[" + std::to_string(node->left->slot_id) + "] ^ ~scratchpad[" + std::to_string(node->right->slot_id) + "] ^ carry_in;\n"
                       "    }\n";
                break;
            }
            default:
                break;
        }
    }

    // 4. Trace forest structure to gather SUM reduction leaves and weights
    uint64_t base_sum = 0;
    std::vector<int64_t> delta_weights;
    std::vector<int> masks_to_popcount;
    
    ir::Node* sum_node = root;
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
                int64_t right_val = 0;
                if (op->right && op->right->kind == ir::NodeKind::CONST) {
                    right_val = static_cast<int64_t>(op->right->const_value);
                }
                base_sum += right_val;
                if (op->left && op->left->kind == ir::NodeKind::CONST) {
                    delta_weights.push_back(static_cast<int64_t>(op->left->const_value) - right_val);
                } else {
                    delta_weights.push_back(static_cast<int64_t>(op->weight) - right_val);
                }
            } else {
                masks_to_popcount.push_back(op->slot_id);
                delta_weights.push_back(op->weight);
            }
        }
    }

    // 5. Append Two-Stage reduction blocks unrolled natively inside the shader!
    msl += "\n    // --- Vectorized Aggregator with Two-Stage reduction ---\n"
           "    int64_t row_sum = " + std::to_string(base_sum) + "LL;\n";
    for (size_t i = 0; i < delta_weights.size(); ++i) {
        msl += "    if ((scratchpad[" + std::to_string(masks_to_popcount[i]) + "] >> bit_idx) & 1) {\n"
               "        row_sum += " + std::to_string(delta_weights[i]) + "LL;\n"
               "    }\n";
    }

    msl += "\n"
           "    // Stage 1: SIMDgroup Reduction via shuffles\n"
           "    uint64_t local_sum = static_cast<uint64_t>(row_sum);\n"
           "    local_sum += shuffle_down_64(local_sum, 16);\n"
           "    local_sum += shuffle_down_64(local_sum, 8);\n"
           "    local_sum += shuffle_down_64(local_sum, 4);\n"
           "    local_sum += shuffle_down_64(local_sum, 2);\n"
           "    local_sum += shuffle_down_64(local_sum, 1);\n"
           "\n"
           "    if (lane_id == 0) {\n"
           "        shared_sums[simd_id] = local_sum;\n"
           "    }\n"
           "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
           "\n"
            "    // Stage 2: Threadgroup Leader write to block-buffered output slot\n"
            "    if (thread_in_group == 0) {\n"
            "        uint64_t block_sum = shared_sums[0] + shared_sums[1];\n"
            "        output_masks[block_idx] = block_sum;\n"
            "    }\n"
            "}\n";

    return msl;
}

} // namespace apex
