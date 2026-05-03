#include <iostream>
#include <cstring>
#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <pthread.h>
#include "apex/compute/bit_slicer.hpp"
#include "apex/compute/column_buffer.hpp"
#include "apex/jit/compiler.hpp"

namespace apex_bench {

// Prevent compiler dead-code elimination
void do_not_optimize(const void* ptr) {
    asm volatile("" : : "r"(ptr) : "memory");
}

// ============================================================================
// PHASE 1: Scalar Reference Implementation (Single-threaded, simple)
// ============================================================================

uint64_t scalar_reference_count(const uint64_t* prices, size_t num_rows, uint64_t threshold) {
    uint64_t count = 0;
    for (size_t i = 0; i < num_rows; i++) {
        if (prices[i] > threshold) {
            count++;
        }
    }
    return count;
}

// ============================================================================
// PHASE 2: Parallel Runner (from Module 5, with results capture per chunk)
// ============================================================================

struct ChunkResult {
    uint64_t chunk_id;
    uint64_t result_mask;
};

struct ParallelResults {
    uint64_t total_matches;
    std::vector<ChunkResult> chunk_results;  // For spot checking
};

ParallelResults parallel_runner_with_results(
    uint64_t* prices,
    size_t num_rows,
    uint64_t threshold,
    int num_threads
) {
    constexpr size_t kChunkSize = 64;
    size_t num_chunks = num_rows / kChunkSize;

    std::atomic<uint64_t> chunk_counter{0};
    std::atomic<uint64_t> total_matches{0};
    std::vector<ChunkResult> chunk_results(num_chunks);
    std::atomic<size_t> chunk_results_idx{0};

    // Compile JIT kernel once on main thread
    apex::jit::JitCompiler compiler;
    auto kernel = compiler.compile_comparison(threshold);
    if (!kernel) {
        std::cerr << "✗ Failed to compile JIT kernel\n";
        return {0, {}};
    }

    // Warm up BitSlicer ISA detection
    {
        apex::compute::BitSlicer warmup_slicer;
        apex::compute::ColumnBuffer warmup_in{}, warmup_out{};
        warmup_slicer.slice(warmup_in, warmup_out);
    }

    auto worker = [&](int) {
        thread_local apex::compute::ColumnBuffer tls_in;
        thread_local apex::compute::ColumnBuffer tls_out;
        thread_local apex::compute::BitSlicer tls_slicer;

#ifdef __APPLE__
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

        uint64_t local_matches = 0;
        std::vector<ChunkResult> local_chunks;

        while (true) {
            uint64_t chunk = chunk_counter.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= num_chunks) break;

            const uint64_t* chunk_ptr = prices + chunk * kChunkSize;

            // Prefetch next chunk
            const uint64_t* next_ptr = chunk_ptr + kChunkSize;
            if (chunk + 1 < num_chunks) {
                __builtin_prefetch(next_ptr,      0, 3);
                __builtin_prefetch(next_ptr + 8,  0, 3);
                __builtin_prefetch(next_ptr + 16, 0, 3);
                __builtin_prefetch(next_ptr + 24, 0, 3);
            }

            // Load chunk
            std::memcpy(tls_in.data, chunk_ptr, kChunkSize * sizeof(uint64_t));

            // BitSlicer transpose
            tls_slicer.slice(tls_in, tls_out);

            // JIT comparison kernel
            uint64_t mask = kernel(tls_out.data);

            // Count matches
            local_matches += static_cast<uint64_t>(__builtin_popcountll(mask));

            // Store result for spot checking (only for first and last chunks)
            if (chunk == 0 || chunk == num_chunks - 1) {
                local_chunks.push_back({chunk, mask});
            }
        }

        total_matches.fetch_add(local_matches, std::memory_order_relaxed);

        // Merge local chunk results
        for (const auto& cr : local_chunks) {
            size_t idx = chunk_results_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx < chunk_results.size()) {
                chunk_results[idx] = cr;
            }
        }
    };

    // Launch threads
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back(worker, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    return {total_matches.load(), chunk_results};
}

// ============================================================================
// PHASE 3: Bit-Level Spot Check
// ============================================================================

bool verify_chunk_results(
    const uint64_t* prices,
    const std::vector<ChunkResult>& chunk_results,
    uint64_t threshold,
    size_t first_chunk_idx,
    size_t last_chunk_idx
) {
    constexpr size_t kChunkSize = 64;
    bool all_correct = true;

    std::cout << "\n=== BIT-LEVEL SPOT CHECK ===\n";

    // Verify first chunk (chunk 0)
    std::cout << "\n--- First 64 Rows (Chunk 0) ---\n";
    const uint64_t* chunk0_ptr = prices + 0 * kChunkSize;
    uint64_t chunk0_mask = 0;

    // Find chunk 0 result in results
    for (const auto& cr : chunk_results) {
        if (cr.chunk_id == 0) {
            chunk0_mask = cr.result_mask;
            break;
        }
    }

    std::cout << "Row | Price  | Expected Bit | Actual Bit | Match\n";
    std::cout << "----+--------+--------------+------------+-------\n";
    for (size_t i = 0; i < kChunkSize; i++) {
        bool expected = chunk0_ptr[i] > threshold;
        bool actual = (chunk0_mask >> i) & 1;
        bool match = (expected == actual);
        if (!match) all_correct = false;

        std::cout << std::setw(3) << i << " | "
                  << std::setw(6) << chunk0_ptr[i] << " | "
                  << std::setw(12) << (expected ? "1" : "0") << " | "
                  << std::setw(10) << (actual ? "1" : "0") << " | "
                  << (match ? "✓" : "✗") << "\n";
    }

    // Verify last chunk
    std::cout << "\n--- Last 64 Rows (Final Chunk) ---\n";
    const uint64_t* last_chunk_ptr = prices + last_chunk_idx * kChunkSize;
    uint64_t last_chunk_mask = 0;

    for (const auto& cr : chunk_results) {
        if (cr.chunk_id == last_chunk_idx) {
            last_chunk_mask = cr.result_mask;
            break;
        }
    }

    std::cout << "Row | Price  | Expected Bit | Actual Bit | Match\n";
    std::cout << "----+--------+--------------+------------+-------\n";
    for (size_t i = 0; i < kChunkSize; i++) {
        bool expected = last_chunk_ptr[i] > threshold;
        bool actual = (last_chunk_mask >> i) & 1;
        bool match = (expected == actual);
        if (!match) all_correct = false;

        std::cout << std::setw(3) << i << " | "
                  << std::setw(6) << last_chunk_ptr[i] << " | "
                  << std::setw(12) << (expected ? "1" : "0") << " | "
                  << std::setw(10) << (actual ? "1" : "0") << " | "
                  << (match ? "✓" : "✗") << "\n";
    }

    return all_correct;
}

} // namespace apex_bench

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  APEX FINAL VERIFICATION: 128M Row Benchmark with Validation  ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";

    constexpr size_t kNumRows = 128'000'000;
    constexpr size_t kAlignBytes = 16 * 1024;
    constexpr uint64_t kThreshold = 25000;
    constexpr int kNumThreads = 4;

    // Allocate 128M prices
    void* raw = nullptr;
    if (posix_memalign(&raw, kAlignBytes, kNumRows * sizeof(uint64_t)) != 0) {
        std::cerr << "✗ Failed to allocate aligned memory\n";
        return 1;
    }
    uint64_t* prices = static_cast<uint64_t*>(raw);

    // Fill with xorshift64 RNG
    std::cout << "\n[SETUP] Generating 128M mock prices...\n";
    {
        uint64_t seed = 0xDEADBEEFCAFEBABEULL;
        for (size_t i = 0; i < kNumRows; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            prices[i] = 10000 + (seed % 50000);
        }
        std::cout << "✓ Generated " << kNumRows << " prices (threshold=" << kThreshold << ")\n";
    }

    // ========================================================================
    // PHASE 1: SCALAR REFERENCE
    // ========================================================================
    std::cout << "\n[PHASE 1] SCALAR REFERENCE (single-threaded baseline)\n";
    auto t_scalar_start = std::chrono::high_resolution_clock::now();
    uint64_t scalar_count = apex_bench::scalar_reference_count(prices, kNumRows, kThreshold);
    auto t_scalar_end = std::chrono::high_resolution_clock::now();
    double scalar_ms = std::chrono::duration<double, std::milli>(t_scalar_end - t_scalar_start).count();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Matches (scalar):  " << scalar_count << "\n";
    std::cout << "  Elapsed time (ms): " << scalar_ms << "\n";

    // ========================================================================
    // PHASE 2: PARALLEL RUNNER
    // ========================================================================
    std::cout << "\n[PHASE 2] PARALLEL RUNNER (4 threads, work-stealing)\n";
    auto t_parallel_start = std::chrono::high_resolution_clock::now();
    auto parallel_results = apex_bench::parallel_runner_with_results(
        prices, kNumRows, kThreshold, kNumThreads
    );
    auto t_parallel_end = std::chrono::high_resolution_clock::now();
    double parallel_ms = std::chrono::duration<double, std::milli>(t_parallel_end - t_parallel_start).count();

    uint64_t parallel_count = parallel_results.total_matches;
    double tps = kNumRows / (parallel_ms / 1000.0);

    std::cout << "  Matches (parallel): " << parallel_count << "\n";
    std::cout << "  Elapsed time (ms):  " << parallel_ms << "\n";
    std::cout << "  TPS:                " << std::fixed << std::setprecision(0)
              << tps << "\n";

    // ========================================================================
    // PHASE 3: VERIFICATION COMPARISON
    // ========================================================================
    std::cout << "\n[PHASE 3] VERIFICATION COMPARISON\n";
    int64_t delta = static_cast<int64_t>(parallel_count) - static_cast<int64_t>(scalar_count);
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Scalar count:      " << scalar_count << "\n";
    std::cout << "  Parallel count:    " << parallel_count << "\n";
    std::cout << "  Delta:             " << delta << "\n";

    if (delta == 0) {
        std::cout << "  ✓ COUNTS MATCH EXACTLY!\n";
    } else {
        std::cout << "  ✗ COUNTS MISMATCH - Delta: " << delta << "\n";
    }

    // ========================================================================
    // PHASE 4: BIT-LEVEL SPOT CHECK (first and last 64 rows)
    // ========================================================================
    size_t num_chunks = kNumRows / 64;
    bool spot_check_passed = apex_bench::verify_chunk_results(
        prices,
        parallel_results.chunk_results,
        kThreshold,
        0,
        num_chunks - 1
    );

    std::cout << "\n[PHASE 4] BIT-LEVEL VERIFICATION RESULT\n";
    if (spot_check_passed) {
        std::cout << "  ✓ All spot-checked bits are correct!\n";
    } else {
        std::cout << "  ✗ Some spot-checked bits are incorrect!\n";
    }

    // ========================================================================
    // FINAL ASSERTION & REPORT
    // ========================================================================
    std::cout << "\n╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                       FINAL REPORT                             ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n  Benchmark Statistics:\n";
    std::cout << "    Scalar time:      " << scalar_ms << " ms\n";
    std::cout << "    Parallel time:    " << parallel_ms << " ms\n";
    std::cout << "    Speedup:          " << (scalar_ms / parallel_ms) << "×\n";
    std::cout << "    TPS achieved:     " << std::fixed << std::setprecision(0)
              << tps << " (" << std::fixed << std::setprecision(1)
              << (tps / 1'000'000'000.0) << " Billion)\n";

    std::cout << "\n  Verification Results:\n";
    std::cout << "    Scalar count:     " << scalar_count << "\n";
    std::cout << "    Parallel count:   " << parallel_count << "\n";
    std::cout << "    Match delta:      " << delta << "\n";
    std::cout << "    Spot check:       " << (spot_check_passed ? "PASS" : "FAIL") << "\n";

    // Exit with error if counts don't match
    if (delta != 0 || !spot_check_passed) {
        std::cout << "\n✗ VERIFICATION FAILED!\n";
        std::free(raw);
        return 1;  // Exit with error code
    }

    std::cout << "\n✓ ALL VERIFICATIONS PASSED - Architecture is Correct!\n";

    // Final output: Print the total matches count
    // Use asm volatile to prevent compiler optimization of the entire logic loop
    std::cout << "\n╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                  FINAL MATCH COUNT VERIFICATION                ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\nTotal Matches (from 128M rows): " << parallel_count << "\n";

    // Ensure the match count is actually used by the compiler
    // This asm volatile barrier prevents dead-code elimination of the entire loop
    asm volatile("" : : "r"(parallel_count) : "memory");

    std::cout << "Barrier: Compiler prevented from optimizing away match counting logic\n";
    std::cout << "\n✓ Final count verified and printed: " << parallel_count << " matches\n\n";

    std::free(raw);
    return 0;
}
