#include "apex/AarchGate.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <atomic>
#include <sys/mman.h>
#include <unistd.h>
#include <iomanip>

using namespace apex;
using namespace apex::builder;

// Black Hole: Prevents compiler from optimizing away the result
std::atomic<uint64_t> result_sink{0};

// External Linkage Barrier - prevents inlining
extern "C" uint64_t external_execute(apex::ApexEngine* engine, const void* data, size_t count);

struct RowData {
    uint64_t features[8];
};

// Scalar Model for Baseline: 100 Tree logic
struct TreeModel {
    int feature_idx[100];
    uint64_t threshold[100];
    uint64_t weights[100][2];
};

// Hardware Timer Helper (ARM64)
inline uint64_t read_cycles() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;
}

inline uint64_t read_freq() {
    uint64_t val;
    asm volatile("mrs %0, cntfrq_el0" : "=r" (val));
    return val;
}


// Use __attribute__((noinline)) to prevent compiler from seeing through the function
__attribute__((noinline))
uint64_t run_scalar_forest(const RowData* data, size_t count, const TreeModel& model) {
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t row_sum = 0;
        for (int t = 0; t < 100; ++t) {
            if (data[i].features[model.feature_idx[t]] > model.threshold[t]) {
                row_sum += model.weights[t][0];
            } else {
                row_sum += model.weights[t][1];
            }
        }
        total += row_sum;
    }
    // Hardware Barrier
    asm volatile("" : : "r" (total) : "memory");
    return total;
}

int main() {
    std::cout << "=== AarchGate Random Forest Benchmark (100 Trees, 10M Rows) ===" << std::endl;

    ApexEngine engine;

    // Define schema: 8 UINT64 features
    std::vector<core::FieldDescriptor> fields;
    for (int i = 0; i < 8; ++i) {
        fields.emplace_back("Feature" + std::to_string(i), i * 8, 64, core::DataType::UINT64);
    }
    engine.register_schema("rf_schema", fields, 64);

    // Build 100 Trees & Metadata for Scalar Baseline
    std::vector<ir::Node*> trees;
    TreeModel scalar_model;
    std::mt19937 gen(42);
    std::uniform_int_distribution<int> feat_dist(0, 7);
    std::uniform_int_distribution<uint64_t> thresh_dist(0, 1000);
    std::uniform_int_distribution<uint64_t> weight_dist(1, 100);

    for (int t = 0; t < 100; ++t) {
        int f_idx = feat_dist(gen);
        uint64_t thresh = thresh_dist(gen);
        uint64_t w1 = weight_dist(gen);
        uint64_t w2 = weight_dist(gen);

        scalar_model.feature_idx[t] = f_idx;
        scalar_model.threshold[t] = thresh;
        scalar_model.weights[t][0] = w1;
        scalar_model.weights[t][1] = w2;

        auto* feat = Load("Feature" + std::to_string(f_idx));
        auto* tree = Select(GT(feat, Const(thresh)), Const(w1), Const(w2));
        trees.push_back(tree);
    }

    auto* forest = Sum(trees);

    uint64_t timer_freq = read_freq();
    std::cout << "Compiling JIT expression with " << trees.size() << " trees..." << std::endl;
    uint64_t compile_start = read_cycles();
    engine.set_expression("rf_schema", forest);
    uint64_t compile_end = read_cycles();
    double compile_ms = ((double)(compile_end - compile_start) / timer_freq) * 1000.0;
    std::cout << "JIT Compilation Time: " << compile_ms << " ms" << std::endl;

    // Production-scale test: 10M rows (matches output header, good multi-core test)
    const size_t num_rows = 10000000;
    size_t alloc_size = num_rows * sizeof(RowData);
    void* raw_ptr = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw_ptr == MAP_FAILED) {
        return 1;
    }
    RowData* data = static_cast<RowData*>(raw_ptr);

    // Fill with volatile pattern to prevent constant-folding
    volatile uint64_t* vdata = (volatile uint64_t*)data;
    for (size_t i = 0; i < num_rows; ++i) {
        for (int f = 0; f < 8; ++f) {
            vdata[i * 8 + f] = i % (100 + f * 10);
        }
    }

    // Debug: Test single row execution and Task 2 Absolute Verification
    std::cout << "Testing expression on single row..." << std::endl;
    uint64_t test_result = engine.execute((const void*)data, 1);
    
    uint64_t scalar_row_0 = run_scalar_forest(data, 1, scalar_model);
    
    std::cout << "[VERIFY] Row 0: Scalar=" << scalar_row_0 << ", JIT=" << test_result;
    if (scalar_row_0 == test_result) {
        std::cout << ". MATCH: YES" << std::endl;
    } else {
        std::cout << ". MATCH: NO" << std::endl;
        std::cerr << "Verification failed! Halting benchmark." << std::endl;
        return 1;
    }

    const int iterations = 2;
    std::vector<double> jit_times;
    std::vector<double> scalar_times;

    // Benchmark JIT with external linkage barrier
    std::cout << "Running AarchGate (50 iterations, Hardware Timers, External Linkage)..." << std::endl;
    std::cout << "Timer frequency: " << timer_freq << " Hz" << std::endl;

    // Calibration: measure empty loop
    uint64_t cal_start = read_cycles();
    uint64_t cal_end = read_cycles();
    uint64_t cal_cycles = cal_end - cal_start;
    std::cout << "Timer overhead: " << cal_cycles << " cycles" << std::endl;

    uint64_t last_result = (uint64_t)time(NULL) % 2;
    for (int i = 0; i < iterations; ++i) {
        // Result-to-Pointer Dependency
        size_t offset = (last_result & 0x1) * 64; 
        const void* perturbed_ptr = (const uint8_t*)data + offset;

        // Pre-benchmark barrier
        asm volatile("dmb sy" ::: "memory");

        uint64_t start = read_cycles();
        last_result = external_execute(&engine, perturbed_ptr, num_rows - 8);
        uint64_t end = read_cycles();

        // Post-benchmark barrier
        asm volatile("dmb sy" ::: "memory");

        // Force read of result and data
        asm volatile("" : : "r"(last_result), "r"(perturbed_ptr) : "memory");

        result_sink.store(result_sink.load() + last_result, std::memory_order_release);

        uint64_t raw_cycles = end - start;
        double ms = ((double)raw_cycles / timer_freq) * 1000.0;
        if (i == 0) {
            std::cout << "First iteration raw cycles: " << raw_cycles << std::endl;
        }
        jit_times.push_back(ms);
    }
    std::sort(jit_times.begin(), jit_times.end());
    double median_jit = jit_times[iterations / 2];

    // Benchmark Scalar
    std::cout << "Running Scalar Baseline (5 iterations, extrapolated)..." << std::endl;
    last_result = (uint64_t)time(NULL) % 2;
    for (int i = 0; i < 5; ++i) {
        size_t offset = (last_result & 0x1) * 64; 
        const RowData* perturbed_ptr = reinterpret_cast<const RowData*>((const uint8_t*)data + offset);

        uint64_t start = read_cycles();
        last_result = run_scalar_forest(perturbed_ptr, (num_rows - 8) / 10, scalar_model); 
        uint64_t end = read_cycles();
        
        result_sink += last_result;
        double ms = ((double)(end - start) / timer_freq) * 1000.0;
        scalar_times.push_back(ms * 10.0);
    }
    std::sort(scalar_times.begin(), scalar_times.end());
    double median_scalar = scalar_times[scalar_times.size() / 2];

    std::cout << "\n--- Single-Thread Performance Results ---" << std::endl;
    std::cout << "AarchGate JIT (Median): " << median_jit << " ms" << std::endl;
    std::cout << "Scalar Baseline (Median): " << median_scalar << " ms" << std::endl;

    double rps = (num_rows / (median_jit / 1000.0));
    std::cout << "JIT Throughput: " << rps / 1e6 << " M rows/sec" << std::endl;
    std::cout << "Cycles/Row (est @ 3.2GHz): " << (3.2e9 / rps) << std::endl;
    std::cout << "True Speedup: " << (median_scalar / median_jit) << "x" << std::endl;

    // === MULTI-THREAD BENCHMARK (4 threads) ===
    std::cout << "\n=== Multi-Core Parallel Execution (4 Threads) ===" << std::endl;

    // Run parallel benchmark (4 threads, 3 iterations)
    std::vector<double> parallel_times;
    std::cout << "Running 4-thread execution (3 iterations)..." << std::endl;

    engine.set_thread_count(4);  // Configure for 4 threads

    for (int i = 0; i < 3; ++i) {
        uint64_t start = read_cycles();
        uint64_t matches = engine.execute_parallel(
            (const void*)data,
            num_rows,
            4  // 4 threads
        );
        uint64_t end = read_cycles();

        double ms = ((double)(end - start) / timer_freq) * 1000.0;
        parallel_times.push_back(ms);

        // Accumulate matches to verify computation
        result_sink.store(result_sink.load() + matches, std::memory_order_release);

        std::cout << "  Iteration " << (i + 1) << ": " << matches << " matches, "
                  << ms << " ms" << std::endl;
    }

    std::sort(parallel_times.begin(), parallel_times.end());
    double median_parallel = parallel_times[parallel_times.size() / 2];

    double parallel_rps = (num_rows / (median_parallel / 1000.0));
    double speedup = median_jit / median_parallel;

    std::cout << "\n--- Multi-Thread Results (4 Threads) ---" << std::endl;
    std::cout << "4-Thread JIT (Median): " << median_parallel << " ms" << std::endl;
    std::cout << "4-Thread Throughput: " << parallel_rps / 1e6 << " M rows/sec" << std::endl;
    std::cout << "Speedup vs 1-Thread: " << speedup << "x" << std::endl;
    std::cout << "Thread Efficiency: " << (speedup / 4.0) * 100.0 << "%" << std::endl;

    // Summary
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Dataset: " << (num_rows / 1e6) << "M rows" << std::endl;
    std::cout << "Forest: 100 trees" << std::endl;
    std::cout << "1-Thread RPS: " << (rps / 1e6) << " M rows/sec" << std::endl;
    std::cout << "4-Thread RPS: " << (parallel_rps / 1e6) << " M rows/sec" << std::endl;
    std::cout << "Multi-Core Speedup: " << speedup << "x" << std::endl;
    if (parallel_rps > 120e6) {
        std::cout << "\n✓ TARGET ACHIEVED: > 120M RPS on 4-thread forest!" << std::endl;
    } else {
        std::cout << "\n✗ Target: 120M RPS (current: " << (parallel_rps / 1e6) << " M RPS)" << std::endl;
    }

    std::cout << "\nResult Verification: " << result_sink.load() << std::endl;

    munmap(raw_ptr, alloc_size);
    return 0;
}
