#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <sys/mman.h>
#include "apex/engine.hpp"

using namespace apex;

int main() {
    const size_t num_rows = 10000000; // 10M Rows
    const size_t num_blocks = num_rows / 64;
    const int num_trees = 100;
    const int num_features = 8;

    std::cout << "=== AarchGate NATIVE SIMD Benchmark (100 Trees, 10M Rows) ===" << std::endl;

    // 1. Prepare Pre-Sliced Data (Native Bit-Planes)
    // block_planes: [num_blocks * num_features * 64] uint64s
    size_t plane_count = num_blocks * num_features * 64;
    uint64_t* bit_planes = (uint64_t*)mmap(nullptr, plane_count * sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (size_t i = 0; i < plane_count; ++i) bit_planes[i] = i; // Dummy data

    // 2. Setup AarchGate
    ApexEngine engine;
    std::vector<core::FieldDescriptor> fields;
    for (int i = 0; i < num_features; ++i) {
        fields.push_back({"f" + std::to_string(i), static_cast<uint32_t>(i * 8), 64, core::DataType::UINT64});
    }
    engine.register_schema("rf_schema", fields, 64);

    using namespace apex::builder;
    std::vector<ir::Node*> trees;
    for (int i = 0; i < num_trees; ++i) {
        auto* t = Select(GT(Load("f" + std::to_string(i % num_features)), Const(50)), Const(100), Const(1));
        trees.push_back(t);
    }
    engine.set_expression("rf_schema", Sum(trees));

    // 3. Native JIT Benchmark (Single-Thread)
    std::cout << "Running Native JIT (No Gathering/Slicing)..." << std::endl;
    std::vector<double> jit_times;
    for (int i = 0; i < 50; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        engine.execute_native("rf_schema", bit_planes, num_blocks);
        auto end = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        jit_times.push_back(static_cast<double>(ns) / 1e6);
    }
    std::sort(jit_times.begin(), jit_times.end());
    double median_jit = jit_times[jit_times.size() / 2];
    double rps = (num_rows / (median_jit / 1000.0)) / 1e6;
    std::cout << "Native JIT Throughput: " << rps << " M rows/sec" << std::endl;

    // 4. Native Parallel JIT
    std::cout << "Running Native Parallel JIT (4 Threads)..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    engine.execute_native_parallel("rf_schema", bit_planes, num_blocks, 4);
    auto end = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double mt_ms = static_cast<double>(ns) / 1e6;
    std::cout << "4-Thread Native Throughput: " << (num_rows / (mt_ms / 1000.0)) / 1e6 << " M rows/sec" << std::endl;

    munmap(bit_planes, plane_count * sizeof(uint64_t));
    return 0;
}
