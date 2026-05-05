// (c) 2024-2026 Suprath PS. All rights reserved.
// Project Apex: Universal JIT-Accelerated Vector Engine (10B+ RPS)
//
// This work is licensed under the Business Source License 1.1 until 2029-05-03,
// transitioning to the Apache License 2.0 thereafter.
// See the LICENSE file in the repository root for the full text.

#include "apex/compute/parallel_runner.hpp"
#include <thread>
#include <array>
#include <algorithm>
#include <cstring>
#include <arm_neon.h>

#ifdef __APPLE__
#include <pthread.h>
#endif

namespace apex::compute {

// Cache-line padded result slot to prevent false sharing between threads
// Stores both the BITPLANE result count and execution statistics
struct alignas(64) PaddedResult {
    uint64_t count = 0;        // Bit-count from BITPLANE kernel (popcnt)
    uint64_t cycles = 0;       // Execution cycles for this thread's work
    uint32_t pad[6];           // Fill to 64 bytes total
};

// ARM64 cycle counter reader
inline uint64_t read_cycles() noexcept {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;
}

// ULL-Compliant: Zero shared mutable state, cache-line padded, work-stealing safe
// Latency target: 64M+ records/sec per thread (656M aggregate on 4 threads)
// Per-thread worker function — completely self-contained, zero shared mutable state
static void worker_thread(
    const uint8_t* base,
    size_t start_row,
    size_t end_row,
    const ParallelRunner::TaskConfig& config,
    PaddedResult* result) noexcept {

#ifdef __APPLE__
    // Hint macOS scheduler to use Performance cores (not Efficiency cores)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    // Per-thread resources — zero heap, zero sharing
    BitSlicer slicer;
    std::array<ColumnBuffer, 8> field_buffers;

    // Use heap-allocated aligned memory for scratchpad to avoid stack alignment issues
    uint64_t* scratchpad = nullptr;
    if (posix_memalign((void**)&scratchpad, 64, 262144 * sizeof(uint64_t)) != 0) {
        result->count = 0;
        result->cycles = 0;
        return;
    }

    // Ensure it's cleared
    if (scratchpad) {
        std::memset(scratchpad, 0, 262144 * sizeof(uint64_t));
    } else {
        result->count = 0;
        result->cycles = 0;
        return;
    }

    const size_t row_stride = config.row_stride;
    const size_t num_fields = config.fields.size();
    uint64_t total_matches = 0;

    // Diagnostic: Ensure fields are populated
    if (num_fields == 0) {
        result->count = 0;
        result->cycles = 0;
        free(scratchpad);
        return;
    }

    // Start cycle counting for hot path (BITPLANE execution)
    uint64_t cycle_start = read_cycles();

    // Process in 64-row blocks
    for (size_t row = start_row; row < end_row; row += 64) {
        const uint8_t* chunk_base = base + row * row_stride;
        size_t rows_in_chunk = std::min(size_t(64), end_row - row);

        // Aggressive prefetch: 3 blocks (192 rows) ahead for better L1 fill
        __builtin_prefetch(chunk_base + 128 * row_stride, 0, 3);
        __builtin_prefetch(chunk_base + 192 * row_stride, 0, 3);
        __builtin_prefetch(chunk_base + 256 * row_stride, 0, 3);

        // Gather + slice all referenced fields
        alignas(64) const uint64_t* field_planes[8];
        std::memset(field_planes, 0, sizeof(field_planes));

        bool use_simd_5 = true; 

        if (use_simd_5) {
            for (size_t r = 0; r < rows_in_chunk; ++r) {
                const uint64_t* row_ptr = reinterpret_cast<const uint64_t*>(chunk_base + r * row_stride);
                uint64x2_t v01 = vld1q_u64(row_ptr);
                uint64x2_t v23 = vld1q_u64(row_ptr + 2);
                uint64_t v4 = row_ptr[4];
                
                field_buffers[0].data[r] = vgetq_lane_u64(v01, 0);
                field_buffers[1].data[r] = vgetq_lane_u64(v01, 1);
                field_buffers[2].data[r] = vgetq_lane_u64(v23, 0);
                field_buffers[3].data[r] = vgetq_lane_u64(v23, 1);
                field_buffers[4].data[r] = v4;
            }
            // Zero-pad
            for (size_t r = rows_in_chunk; r < 64; ++r) {
                for (int f = 0; f < 5; ++f) field_buffers[f].data[r] = 0;
            }
            for (int f = 0; f < 5; ++f) {
                if (config.mode == ExecutionMode::BIT_SLICED) {
                    slicer.slice_n(field_buffers[f].data, 64, const_cast<uint64_t*>(field_buffers[f].data), config.active_bits);
                }
                field_planes[f] = field_buffers[f].data;
            }
            // Tail fields
            for (size_t f = 5; f < num_fields && f < 8; ++f) {
                const size_t offset = config.fields[f]->offset;
                for (size_t r = 0; r < rows_in_chunk; ++r) {
                    std::memcpy(&field_buffers[f].data[r], chunk_base + r * row_stride + offset, 8);
                }
                for (size_t r = rows_in_chunk; r < 64; ++r) field_buffers[f].data[r] = 0;
                
                if (config.mode == ExecutionMode::BIT_SLICED) {
                    slicer.slice_n(field_buffers[f].data, 64, const_cast<uint64_t*>(field_buffers[f].data), config.active_bits);
                }
                field_planes[f] = field_buffers[f].data;
            }
        } else {
            for (size_t f = 0; f < num_fields && f < 8; ++f) {
                const size_t offset = config.fields[f]->offset;
                for (size_t r = 0; r < rows_in_chunk; ++r) {
                    std::memcpy(&field_buffers[f].data[r], chunk_base + r * row_stride + offset, 8);
                }
                for (size_t r = rows_in_chunk; r < 64; ++r) field_buffers[f].data[r] = 0;

                if (config.mode == ExecutionMode::BIT_SLICED) {
                    slicer.slice_n(field_buffers[f].data, 64, const_cast<uint64_t*>(field_buffers[f].data), config.active_bits);
                }
                field_planes[f] = field_buffers[f].data;
            }
        }

        // Execute JIT kernel
        uint64_t kernel_res = config.kernel(field_planes, scratchpad);

        uint64_t rows_mask = (rows_in_chunk == 64) ? ~0ULL : (1ULL << rows_in_chunk) - 1;

        if (config.result_kind == 1) { // BITMASK
            total_matches += static_cast<uint64_t>(__builtin_popcountll(kernel_res & rows_mask));
        } else if (!config.delta_weights.empty()) { // HYBRID POPCOUNT
            int64_t total_sum = static_cast<int64_t>(config.base_sum) * rows_in_chunk;
            for (size_t i = 0; i < config.delta_weights.size(); ++i) {
                int mask_slot = config.masks_to_popcount[i];
                int64_t pop = __builtin_popcountll(scratchpad[mask_slot] & rows_mask);
                total_sum += pop * config.delta_weights[i];
            }
            total_matches += static_cast<uint64_t>(total_sum);
        } else { // BITPLANE
            for (int i = 0; i < 64; ++i) {
                total_matches += (static_cast<uint64_t>(__builtin_popcountll(scratchpad[i] & rows_mask))) << i;
            }
        }
    }

    uint64_t cycle_end = read_cycles();

    free(scratchpad);
    result->count = total_matches;
    result->cycles = cycle_end - cycle_start;
}

// ULL-Compliant: Work distribution orchestrator, thread-safe
// Non-hot-path: spawns hot worker_thread() tasks
uint64_t ParallelRunner::run(
    const void* data_ptr,
    size_t total_rows,
    const TaskConfig& config,
    int num_threads) noexcept {

    if (num_threads <= 0) num_threads = 1;
    if (total_rows == 0) return 0;

    const uint8_t* base = static_cast<const uint8_t*>(data_ptr);

    // Align chunk boundaries to 64-row blocks
    size_t rows_per_thread = ((total_rows / num_threads) / 64) * 64;
    if (rows_per_thread == 0) rows_per_thread = 64;

    // Cache-line padded result slots — no false sharing
    std::vector<PaddedResult> results(num_threads);
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        size_t start = t * rows_per_thread;
        size_t end = (t == num_threads - 1) ? total_rows : start + rows_per_thread;

        if (start >= total_rows) break;

        threads.emplace_back(worker_thread,
                             base, start, end,
                             std::cref(config),
                             &results[t]);
    }

    // Wait for all workers
    for (auto& t : threads) {
        t.join();
    }

    // Aggregate results
    uint64_t total = 0;
    for (int t = 0; t < num_threads; ++t) {
        total += results[t].count;
    }
    return total;
}

// Execute with detailed execution statistics (cycles per row, etc)
ParallelRunner::ExecutionStats ParallelRunner::run_with_stats(
    const void* data_ptr,
    size_t total_rows,
    const TaskConfig& config,
    int num_threads) noexcept {

    if (num_threads <= 0) num_threads = 1;
    if (total_rows == 0) return {0, 0, 0};

    const uint8_t* base = static_cast<const uint8_t*>(data_ptr);

    // Align chunk boundaries to 64-row blocks
    size_t rows_per_thread = ((total_rows / num_threads) / 64) * 64;
    if (rows_per_thread == 0) rows_per_thread = 64;

    // Cache-line padded result slots — no false sharing
    std::vector<PaddedResult> results(num_threads);
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        size_t start = t * rows_per_thread;
        size_t end = (t == num_threads - 1) ? total_rows : start + rows_per_thread;

        if (start >= total_rows) break;

        threads.emplace_back(worker_thread,
                             base, start, end,
                             std::cref(config),
                             &results[t]);
    }

    // Wait for all workers
    for (auto& t : threads) {
        t.join();
    }

    // Aggregate results and compute statistics
    uint64_t total_matches = 0;
    uint64_t total_cycles = 0;
    for (int t = 0; t < num_threads; ++t) {
        total_matches += results[t].count;
        total_cycles += results[t].cycles;
    }

    uint64_t avg_cycles = (total_rows > 0) ? (total_cycles / total_rows) : 0;
    return {total_matches, total_cycles, avg_cycles};
}

} // namespace apex::compute
