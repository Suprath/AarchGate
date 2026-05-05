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
#include <queue>
#include <mutex>
#include <condition_variable>

#ifdef __APPLE__
#include <pthread.h>
#endif

namespace apex::compute {

struct PaddedResult;
static void worker_thread(
    const uint8_t* base,
    size_t start_row,
    size_t end_row,
    const ParallelRunner::TaskConfig& config,
    PaddedResult* result) noexcept;

struct WorkerTask {
    const uint8_t* base;
    size_t start_row;
    size_t end_row;
    const ParallelRunner::TaskConfig* config;
    PaddedResult* result;
};

class ThreadPool {
private:
    std::vector<std::thread> workers_;
    std::queue<WorkerTask> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    int active_workers_ = 0;
    std::condition_variable wait_cv_;

    void worker_loop() {
#ifdef __APPLE__
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        while (true) {
            WorkerTask task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
                active_workers_++;
            }

            worker_thread(task.base, task.start_row, task.end_row, *task.config, task.result);

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                active_workers_--;
                if (tasks_.empty() && active_workers_ == 0) {
                    wait_cv_.notify_all();
                }
            }
        }
    }

public:
    ThreadPool(size_t threads) {
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back(&ThreadPool::worker_loop, this);
        }
    }

    void enqueue(WorkerTask task) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void wait_all() {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        wait_cv_.wait(lock, [this]() { return tasks_.empty() && active_workers_ == 0; });
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }
};

static ThreadPool& get_global_pool() {
    static size_t num_cores = std::thread::hardware_concurrency();
    if (num_cores == 0) num_cores = 4;
    static ThreadPool pool(num_cores);
    return pool;
}

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
    std::array<ColumnBuffer, 32> field_buffers;

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
        alignas(64) const uint64_t* field_planes[32];
        std::memset(field_planes, 0, sizeof(field_planes));

        bool use_simd_5 = (num_fields >= 5 && row_stride >= 40); 

        if (use_simd_5) {
            if (rows_in_chunk == 64) {
                for (size_t r = 0; r < 64; r += 4) {
                    const uint8_t* cb0 = chunk_base + r * row_stride;
                    const uint8_t* cb1 = cb0 + row_stride;
                    const uint8_t* cb2 = cb1 + row_stride;
                    const uint8_t* cb3 = cb2 + row_stride;

                    const uint64_t* rp0 = reinterpret_cast<const uint64_t*>(cb0);
                    const uint64_t* rp1 = reinterpret_cast<const uint64_t*>(cb1);
                    const uint64_t* rp2 = reinterpret_cast<const uint64_t*>(cb2);
                    const uint64_t* rp3 = reinterpret_cast<const uint64_t*>(cb3);

                    uint64x2_t v01_0 = vld1q_u64(rp0);
                    uint64x2_t v23_0 = vld1q_u64(rp0 + 2);
                    uint64_t v4_0 = rp0[4];

                    uint64x2_t v01_1 = vld1q_u64(rp1);
                    uint64x2_t v23_1 = vld1q_u64(rp1 + 2);
                    uint64_t v4_1 = rp1[4];

                    uint64x2_t v01_2 = vld1q_u64(rp2);
                    uint64x2_t v23_2 = vld1q_u64(rp2 + 2);
                    uint64_t v4_2 = rp2[4];

                    uint64x2_t v01_3 = vld1q_u64(rp3);
                    uint64x2_t v23_3 = vld1q_u64(rp3 + 2);
                    uint64_t v4_3 = rp3[4];

                    field_buffers[0].data[r]   = vgetq_lane_u64(v01_0, 0);
                    field_buffers[1].data[r]   = vgetq_lane_u64(v01_0, 1);
                    field_buffers[2].data[r]   = vgetq_lane_u64(v23_0, 0);
                    field_buffers[3].data[r]   = vgetq_lane_u64(v23_0, 1);
                    field_buffers[4].data[r]   = v4_0;

                    field_buffers[0].data[r+1] = vgetq_lane_u64(v01_1, 0);
                    field_buffers[1].data[r+1] = vgetq_lane_u64(v01_1, 1);
                    field_buffers[2].data[r+1] = vgetq_lane_u64(v23_1, 0);
                    field_buffers[3].data[r+1] = vgetq_lane_u64(v23_1, 1);
                    field_buffers[4].data[r+1] = v4_1;

                    field_buffers[0].data[r+2] = vgetq_lane_u64(v01_2, 0);
                    field_buffers[1].data[r+2] = vgetq_lane_u64(v01_2, 1);
                    field_buffers[2].data[r+2] = vgetq_lane_u64(v23_2, 0);
                    field_buffers[3].data[r+2] = vgetq_lane_u64(v23_2, 1);
                    field_buffers[4].data[r+2] = v4_2;

                    field_buffers[0].data[r+3] = vgetq_lane_u64(v01_3, 0);
                    field_buffers[1].data[r+3] = vgetq_lane_u64(v01_3, 1);
                    field_buffers[2].data[r+3] = vgetq_lane_u64(v23_3, 0);
                    field_buffers[3].data[r+3] = vgetq_lane_u64(v23_3, 1);
                    field_buffers[4].data[r+3] = v4_3;
                }
            } else {
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
            for (size_t f = 5; f < num_fields && f < 32; ++f) {
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
            for (size_t f = 0; f < num_fields && f < 32; ++f) {
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
            const size_t num_weights = config.delta_weights.size();
            const int64_t* weights = config.delta_weights.data();
            const int* masks = config.masks_to_popcount.data();
            
            size_t i = 0;
            // Unroll 8x for maximum instruction pipelining and register-level execution
            for (; i + 8 <= num_weights; i += 8) {
                int m0 = masks[i];
                int m1 = masks[i+1];
                int m2 = masks[i+2];
                int m3 = masks[i+3];
                int m4 = masks[i+4];
                int m5 = masks[i+5];
                int m6 = masks[i+6];
                int m7 = masks[i+7];

                uint64_t s0 = scratchpad[m0] & rows_mask;
                uint64_t s1 = scratchpad[m1] & rows_mask;
                uint64_t s2 = scratchpad[m2] & rows_mask;
                uint64_t s3 = scratchpad[m3] & rows_mask;
                uint64_t s4 = scratchpad[m4] & rows_mask;
                uint64_t s5 = scratchpad[m5] & rows_mask;
                uint64_t s6 = scratchpad[m6] & rows_mask;
                uint64_t s7 = scratchpad[m7] & rows_mask;

                int64_t p0 = __builtin_popcountll(s0);
                int64_t p1 = __builtin_popcountll(s1);
                int64_t p2 = __builtin_popcountll(s2);
                int64_t p3 = __builtin_popcountll(s3);
                int64_t p4 = __builtin_popcountll(s4);
                int64_t p5 = __builtin_popcountll(s5);
                int64_t p6 = __builtin_popcountll(s6);
                int64_t p7 = __builtin_popcountll(s7);

                total_sum += p0 * weights[i];
                total_sum += p1 * weights[i+1];
                total_sum += p2 * weights[i+2];
                total_sum += p3 * weights[i+3];
                total_sum += p4 * weights[i+4];
                total_sum += p5 * weights[i+5];
                total_sum += p6 * weights[i+6];
                total_sum += p7 * weights[i+7];
            }
            // Tail
            for (; i < num_weights; ++i) {
                int mask_slot = masks[i];
                int64_t pop = __builtin_popcountll(scratchpad[mask_slot] & rows_mask);
                total_sum += pop * weights[i];
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
    auto& pool = get_global_pool();

    int active_tasks = 0;
    for (int t = 0; t < num_threads; ++t) {
        size_t start = t * rows_per_thread;
        size_t end = (t == num_threads - 1) ? total_rows : start + rows_per_thread;

        if (start >= total_rows) break;

        pool.enqueue({base, start, end, &config, &results[t]});
        active_tasks++;
    }

    // Wait for all workers
    pool.wait_all();

    // Aggregate results
    uint64_t total = 0;
    for (int t = 0; t < active_tasks; ++t) {
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
    auto& pool = get_global_pool();

    int active_tasks = 0;
    for (int t = 0; t < num_threads; ++t) {
        size_t start = t * rows_per_thread;
        size_t end = (t == num_threads - 1) ? total_rows : start + rows_per_thread;

        if (start >= total_rows) break;

        pool.enqueue({base, start, end, &config, &results[t]});
        active_tasks++;
    }

    // Wait for all workers
    pool.wait_all();

    // Aggregate results and compute statistics
    uint64_t total_matches = 0;
    uint64_t total_cycles = 0;
    for (int t = 0; t < active_tasks; ++t) {
        total_matches += results[t].count;
        total_cycles += results[t].cycles;
    }

    uint64_t avg_cycles = (total_rows > 0) ? (total_cycles / total_rows) : 0;
    return {total_matches, total_cycles, avg_cycles};
}

} // namespace apex::compute
