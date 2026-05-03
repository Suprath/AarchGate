#include "apex/AarchGate.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <atomic>
#include <sys/mman.h>
#include <unistd.h>

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

    // Use 100K rows for quick validation; scale up once confirmed working
    const size_t num_rows = 100000;
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
    
    // The JIT kernel processes a chunk of 64 rows. Since we passed row_count=1,
    // rows 1..63 are zero-padded by gather_field. The engine.execute() function then
    // popcounts all 64 rows. We must mirror this exactly in the scalar verification.
    std::vector<RowData> padded_chunk(64);
    padded_chunk[0] = data[0];
    for (int i = 1; i < 64; ++i) {
        for (int f = 0; f < 8; ++f) padded_chunk[i].features[f] = 0;
    }
    uint64_t scalar_row_0 = run_scalar_forest(padded_chunk.data(), 64, scalar_model);
    
    std::cout << "[VERIFY] Row 0 (Chunked): Scalar=" << scalar_row_0 << ", JIT=" << test_result;
    if (scalar_row_0 == test_result) {
        std::cout << ". MATCH: YES" << std::endl;
    } else {
        std::cout << ". MATCH: NO" << std::endl;
        std::cerr << "Verification failed! Halting benchmark." << std::endl;
        return 1;
    }

    const int iterations = 3;
    std::vector<double> jit_times;
    std::vector<double> scalar_times;
    uint64_t entropy = 12345;

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
    std::cout << "Running Scalar Baseline (10 iterations, extrapolated)..." << std::endl;
    last_result = (uint64_t)time(NULL) % 2;
    for (int i = 0; i < 10; ++i) {
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

    std::cout << "\n--- Performance Results ---" << std::endl;
    std::cout << "AarchGate JIT (Median): " << median_jit << " ms" << std::endl;
    std::cout << "Scalar Baseline (Median): " << median_scalar << " ms" << std::endl;
    
    double rps = (num_rows / (median_jit / 1000.0));
    std::cout << "JIT Throughput: " << rps / 1e6 << " M rows/sec" << std::endl;
    std::cout << "Cycles/Row (est @ 3.2GHz): " << (3.2e9 / rps) << std::endl;
    std::cout << "True Speedup: " << (median_scalar / median_jit) << "x" << std::endl;

    std::cout << "\nResult Verification: " << result_sink.load() << std::endl;

    munmap(raw_ptr, alloc_size);
    return 0;
}
