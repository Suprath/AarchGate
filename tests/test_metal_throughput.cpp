// (c) 2024-2026 Suprath PS. All rights reserved.
// Project AarchGate: Universal JIT-Accelerated Vector Engine (10B+ RPS)
//
// Verification & Performance Benchmark: AarchGate-Metal Throughput Tier
// Goal: Measure physical limits of the Apple Silicon Unified Memory architecture.

#include "apex/AarchGate.hpp"
#include "apex/jit/ir.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <cassert>
#include <iomanip>

using namespace apex;
using namespace apex::builder;

struct TaxiRecord {
    uint64_t trip_distance;
    uint64_t passenger_count;
    uint64_t fare_amount;
    uint64_t tip_amount;
    uint64_t tolls_amount;
};

int main() {
    std::cout << "\n==========================================================" << std::endl;
    std::cout << "    AARCHGATE-METAL SILICON CEILING PERFORMANCE BENCHMARK " << std::endl;
    std::cout << "==========================================================\n" << std::endl;

    // 1. Build common schema definitions
    std::vector<core::FieldDescriptor> fields;
    fields.emplace_back("trip_distance",    0,  64, core::DataType::UINT64);
    fields.emplace_back("passenger_count", 8,  64, core::DataType::UINT64);
    fields.emplace_back("fare_amount",     16, 64, core::DataType::UINT64);
    fields.emplace_back("tip_amount",      24, 64, core::DataType::UINT64);
    fields.emplace_back("tolls_amount",    32, 64, core::DataType::UINT64);

    // 2. Dynamically construct a 100-tree XGBoost Decision Forest AST
    std::cout << "[INFO] Constructing 100-tree decision forest IR model..." << std::endl;
    std::vector<ir::Node*> trees;
    std::vector<std::string> field_names = {
        "trip_distance", "passenger_count", "fare_amount", "tip_amount", "tolls_amount"
    };

    for (int t = 0; t < 100; ++t) {
        std::string field_name = field_names[t % 5];
        uint64_t threshold = 10 + t * 5;
        uint64_t weight_gt = 50 + t;
        uint64_t weight_le = 10;

        auto* load_node = Load(field_name.c_str());
        auto* comp_node = GT(load_node, Const(threshold));
        auto* select_node = Select(comp_node, Const(weight_gt), Const(weight_le));
        trees.push_back(select_node);
    }
    auto* forest_root = Sum(trees);

    // 3. Initialize separate engine instances for CPU and GPU execution paths
    ApexEngine engine_cpu;
    engine_cpu.register_schema("nyc_taxi", fields, sizeof(TaxiRecord));
    engine_cpu.set_expression("nyc_taxi", forest_root, ExecutionMode::BIT_SLICED);

    ApexEngine engine_gpu;
    engine_gpu.register_schema("nyc_taxi", fields, sizeof(TaxiRecord));
    engine_gpu.set_expression("nyc_taxi", forest_root, ExecutionMode::GPU_THROUGHPUT);

    // 4. Generate structured high-dimensional database records (10 Million rows)
    const size_t num_rows = 10000000;
    std::cout << "[INFO] Generating " << num_rows << " database rows (" 
              << (num_rows * sizeof(TaxiRecord) / (1024 * 1024)) << " MB memory layout)..." << std::endl;

    std::vector<TaxiRecord> records(num_rows);
    for (size_t i = 0; i < num_rows; ++i) {
        records[i].trip_distance    = i % 120; // 0-119
        records[i].passenger_count   = i % 6;   // 0-5
        records[i].fare_amount       = i % 300; // 0-299
        records[i].tip_amount        = i % 70;  // 0-69
        records[i].tolls_amount      = i % 30;  // 0-29
    }

    // 5. Evaluate scalar reference values on CPU
    std::cout << "[INFO] Pre-calculating scalar reference sum on Host..." << std::endl;
    uint64_t reference_sum = 0;
    for (size_t i = 0; i < num_rows; ++i) {
        uint64_t row_sum = 0;
        for (int t = 0; t < 100; ++t) {
            uint64_t val = 0;
            if (t % 5 == 0) val = records[i].trip_distance;
            else if (t % 5 == 1) val = records[i].passenger_count;
            else if (t % 5 == 2) val = records[i].fare_amount;
            else if (t % 5 == 3) val = records[i].tip_amount;
            else val = records[i].tolls_amount;

            uint64_t threshold = 10 + t * 5;
            row_sum += (val > threshold) ? (50 + t) : 10;
        }
        reference_sum += row_sum;
    }

    // 6. Benchmark CPU JIT engine
    std::cout << "[BENCHMARK] Executing 8-core CPU JIT engine..." << std::endl;
    auto cpu_start = std::chrono::high_resolution_clock::now();
    uint64_t cpu_result = engine_cpu.execute(records.data(), num_rows);
    auto cpu_end = std::chrono::high_resolution_clock::now();
    double cpu_seconds = std::chrono::duration<double>(cpu_end - cpu_start).count();
    double cpu_rps = num_rows / cpu_seconds;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  - CPU Latency: " << (cpu_seconds * 1000.0) << " ms" << std::endl;
    std::cout << "  - CPU Throughput: " << (cpu_rps / 1000000.0) << " Million Rows/sec (RPS)" << std::endl;
    std::cout << "  - CPU Bit-Perfect Match: " << (cpu_result == reference_sum ? "YES" : "NO") << std::endl;

    // 7. Benchmark Apple Silicon GPU via Metal API (Zero-Copy Bridge + MSL)
    std::cout << "[BENCHMARK] Executing unified-memory Metal GPU engine..." << std::endl;
    
    // Run once to warm up shader compilation and dynamic pipeline creation
    engine_gpu.execute(records.data(), num_rows);

    auto gpu_start = std::chrono::high_resolution_clock::now();
    uint64_t gpu_result = engine_gpu.execute(records.data(), num_rows);
    auto gpu_end = std::chrono::high_resolution_clock::now();
    double gpu_seconds = std::chrono::duration<double>(gpu_end - gpu_start).count();
    double gpu_rps = num_rows / gpu_seconds;

    std::cout << "  - GPU Latency: " << (gpu_seconds * 1000.0) << " ms" << std::endl;
    std::cout << "  - GPU Throughput: " << (gpu_rps / 1000000.0) << " Million Rows/sec (RPS)" << std::endl;
    std::cout << "  - GPU Forest Decision Throughput: " << ((gpu_rps * 100.0) / 1000000000.0) << " Billion Decisions/sec" << std::endl;
    std::cout << "  - GPU Bit-Perfect Match: " << (gpu_result == reference_sum ? "YES" : "NO") << std::endl;

    // 8. Scale to physical Silicon Limits (100M continuous records)
    std::cout << "[INFO] Testing physical Silicon Ceiling (100M continuous records)..." << std::endl;
    const int num_iterations = 10;
    auto scale_start = std::chrono::high_resolution_clock::now();
    uint64_t total_scaled_sum = 0;
    for (int iter = 0; iter < num_iterations; ++iter) {
        total_scaled_sum += engine_gpu.execute(records.data(), num_rows);
    }
    auto scale_end = std::chrono::high_resolution_clock::now();
    double scale_seconds = std::chrono::duration<double>(scale_end - scale_start).count();
    double scale_rps = (num_rows * num_iterations) / scale_seconds;

    std::cout << "\n==========================================================" << std::endl;
    std::cout << "    FINAL RESULTS SUMMARY" << std::endl;
    std::cout << "==========================================================" << std::endl;
    std::cout << "  Reference Row Sum: " << reference_sum << std::endl;
    std::cout << "  CPU JIT Row Sum:   " << cpu_result << std::endl;
    std::cout << "  Metal GPU Row Sum: " << gpu_result << std::endl;
    std::cout << "  Scaled 100M Records Total: " << total_scaled_sum << std::endl;
    std::cout << "  100M Record Duration:      " << scale_seconds << " seconds" << std::endl;
    std::cout << "  Sustained Peak Throughput: " << (scale_rps / 1000000.0) << " Million Rows/sec" << std::endl;
    std::cout << "  Total Tree Evaluations:    " << ((scale_rps * 100.0) / 1000000000.0) << " Billion Tree-Evals/sec" << std::endl;
    std::cout << "==========================================================\n" << std::endl;

    if (cpu_result == reference_sum && gpu_result == reference_sum) {
        std::cout << ">>> ALL VERIFICATION SUITES PASSED SUCCESSFULLY! SILICON LEVEL PARITY DETECTED." << std::endl;
        return 0;
    } else {
        std::cout << ">>> VERIFICATION FAILURE DETECTED!" << std::endl;
        return 1;
    }
}
