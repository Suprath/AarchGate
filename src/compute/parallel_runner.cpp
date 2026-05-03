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

#ifdef __APPLE__
#include <pthread.h>
#endif

namespace apex::compute {

// Cache-line padded result slot to prevent false sharing between threads
struct alignas(64) PaddedResult {
    uint64_t count = 0;
    char pad[56];  // Fill to 64 bytes
};

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
    if (posix_memalign((void**)&scratchpad, 64, 8 * 64 * sizeof(uint64_t)) != 0) {
        result->count = 0; // Ensure count is initialized on failure
        return;
    }
    
    // Ensure it's cleared
    if (scratchpad) {
        std::memset(scratchpad, 0, 8 * 64 * sizeof(uint64_t));
    } else {
        result->count = 0;
        return;
    }

    const size_t row_stride = config.row_stride;
    const size_t num_fields = config.fields.size();
    uint64_t total_matches = 0;

    // Process in 64-row blocks
    for (size_t row = start_row; row < end_row; row += 64) {
        const uint8_t* chunk_base = base + row * row_stride;
        size_t rows_in_chunk = std::min(size_t(64), end_row - row);

        // Prefetch 2 blocks (128 rows) ahead — fills L1 before we need it
        __builtin_prefetch(chunk_base + 128 * row_stride, 0, 3);
        __builtin_prefetch(chunk_base + 192 * row_stride, 0, 3);

        // Gather + slice all referenced fields
        alignas(64) const uint64_t* field_planes[8];
        std::memset(field_planes, 0, sizeof(field_planes));
        
        for (size_t f = 0; f < num_fields && f < 8; ++f) {
            const size_t offset = config.fields[f]->offset;

            // 4x unrolled gather
            size_t i = 0;
            for (; i + 4 <= rows_in_chunk; i += 4) {
                uint64_t v0, v1, v2, v3;
                std::memcpy(&v0, chunk_base + (i + 0) * row_stride + offset, 8);
                std::memcpy(&v1, chunk_base + (i + 1) * row_stride + offset, 8);
                std::memcpy(&v2, chunk_base + (i + 2) * row_stride + offset, 8);
                std::memcpy(&v3, chunk_base + (i + 3) * row_stride + offset, 8);
                field_buffers[f].data[i + 0] = v0;
                field_buffers[f].data[i + 1] = v1;
                field_buffers[f].data[i + 2] = v2;
                field_buffers[f].data[i + 3] = v3;
            }
            for (; i < rows_in_chunk; i++) {
                std::memcpy(&field_buffers[f].data[i], chunk_base + i * row_stride + offset, 8);
            }
            for (; i < 64; i++) {
                field_buffers[f].data[i] = 0;
            }

            if (config.mode == ExecutionMode::BIT_SLICED) {
                slicer.slice(field_buffers[f], field_buffers[f]);
            }
            field_planes[f] = field_buffers[f].data;
        }

        // Execute JIT kernel
        uint64_t mask = config.kernel(field_planes, scratchpad);
        total_matches += static_cast<uint64_t>(__builtin_popcountll(mask));
    }

    free(scratchpad);
    result->count = total_matches;
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

} // namespace apex::compute
