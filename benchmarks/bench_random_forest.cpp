#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <sys/mman.h>
#include "apex/engine.hpp"
#include "apex/jit/ir.hpp"

using namespace apex;

struct RowData {
    uint64_t features[8];
};

struct DecisionTree {
    int feature_idx;
    uint64_t threshold;
    uint32_t weight_true;
    uint32_t weight_false;
};

// Use ARM64 Hardware Cycle Counter
static inline uint64_t read_cycles() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;
}

uint64_t run_scalar_forest(const RowData* data, size_t num_rows, const std::vector<DecisionTree>& forest) {
    uint64_t total_sum = 0;
    for (size_t i = 0; i < num_rows; ++i) {
        uint64_t row_sum = 0;
        for (const auto& tree : forest) {
            if (data[i].features[tree.feature_idx] > tree.threshold) {
                row_sum += tree.weight_true;
            } else {
                row_sum += tree.weight_false;
            }
        }
        total_sum += row_sum;
    }
    return total_sum;
}

int main() {
    const size_t num_rows = 10000000; // 10M Rows for fast reliable timing
    const int num_trees = 100;
    const int num_features = 8;
    const double cpu_freq = 24000000.0; // Apple M3 Virtual Timer Frequency

    std::cout << "=== AarchGate Random Forest Benchmark (100 Trees, 10M Rows) ===" << std::endl;

    size_t alloc_size = num_rows * sizeof(RowData);
    RowData* data = (RowData*)mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (size_t i = 0; i < num_rows; ++i) {
        for (int f = 0; f < num_features; ++f) {
            data[i].features[f] = i % (100 + f * 10);
        }
    }

    std::vector<DecisionTree> model;
    for (int i = 0; i < num_trees; ++i) {
        model.push_back({i % num_features, static_cast<uint64_t>(50 + i % 50), static_cast<uint32_t>(100 + i), static_cast<uint32_t>(i)});
    }

    ApexEngine engine;
    std::vector<core::FieldDescriptor> fields;
    for (int i = 0; i < num_features; ++i) {
        fields.push_back({"f" + std::to_string(i), static_cast<uint32_t>(i * 8), 64, core::DataType::UINT64});
    }
    engine.register_schema("rf_schema", fields, 64);

    using namespace apex::builder;
    std::vector<ir::Node*> trees;
    for (const auto& tree : model) {
        auto* t = Select(GT(Load("f" + std::to_string(tree.feature_idx)), Const(tree.threshold)),
                         Const(tree.weight_true), Const(tree.weight_false));
        trees.push_back(t);
    }
    auto* forest_expr = Sum(trees);
    engine.set_expression("rf_schema", forest_expr);

    // Verification
    uint64_t scalar_res = run_scalar_forest(data, 1, model);
    uint64_t jit_res = engine.execute(data, 1);
    std::cout << "[VERIFY] Row 0: Scalar=" << scalar_res << ", JIT=" << jit_res << " MATCH: " << (scalar_res == jit_res ? "YES" : "NO") << std::endl;

    // Single-Thread JIT Benchmark
    std::vector<uint64_t> jit_cycles;
    for (int i = 0; i < 20; ++i) {
        uint64_t start = read_cycles();
        engine.execute(data, num_rows);
        uint64_t end = read_cycles();
        jit_cycles.push_back(end - start);
    }
    std::sort(jit_cycles.begin(), jit_cycles.end());
    uint64_t median_jit_cycles = jit_cycles[jit_cycles.size() / 2];
    double jit_ms = (median_jit_cycles / cpu_freq) * 1000.0;
    double rps_jit = (num_rows / (jit_ms / 1000.0)) / 1e6;
    std::cout << "AarchGate JIT (Median): " << jit_ms << " ms (" << rps_jit << " M rows/sec)" << std::endl;

    // Scalar Baseline
    std::vector<uint64_t> scalar_cycles;
    for (int i = 0; i < 5; ++i) {
        uint64_t start = read_cycles();
        run_scalar_forest(data, num_rows, model);
        uint64_t end = read_cycles();
        scalar_cycles.push_back(end - start);
    }
    std::sort(scalar_cycles.begin(), scalar_cycles.end());
    uint64_t median_scalar_cycles = scalar_cycles[scalar_cycles.size() / 2];
    double scalar_ms = (median_scalar_cycles / cpu_freq) * 1000.0;
    double rps_scalar = (num_rows / (scalar_ms / 1000.0)) / 1e6;
    std::cout << "Scalar Baseline: " << scalar_ms << " ms (" << rps_scalar << " M rows/sec)" << std::endl;
    std::cout << "Speedup: " << (scalar_ms / jit_ms) << "x" << std::endl;

    // Parallel
    uint64_t start = read_cycles();
    engine.execute_parallel(data, num_rows, 4);
    uint64_t end = read_cycles();
    double pt_ms = ((end - start) / cpu_freq) * 1000.0;
    std::cout << "4-Thread JIT: " << pt_ms << " ms (" << (num_rows / (pt_ms / 1000.0)) / 1e6 << " M rows/sec)" << std::endl;

    munmap(data, alloc_size);
    return 0;
}
