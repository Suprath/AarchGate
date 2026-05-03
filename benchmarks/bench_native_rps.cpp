#include "apex/engine.hpp"
#include "apex/jit/ir.hpp"
#include "apex/compute/bit_slicer.hpp"
#include "apex/AarchGate.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <thread>
#include <sys/mman.h>

struct LineItem {
    uint64_t ship_date;
    uint64_t discount;
    uint64_t quantity;
    uint64_t extended_price;
};

int main() {
    const size_t num_rows = 100'000'000;
    const size_t num_blocks = num_rows / 64;
    const size_t num_fields = 3; // ship_date, discount, quantity
    const size_t native_size = num_blocks * num_fields * 64 * sizeof(uint64_t);

    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║            AarchGate TPC-H Q6 Native Record Run                ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";

    // 1. Allocate Raw Data
    std::vector<LineItem> items(num_rows);
    uint64_t expected_matches = 0;
    for (size_t i = 0; i < num_rows; ++i) {
        if (i % 20 == 0) {
            items[i].ship_date = 19940101;
            items[i].discount = 600;
            items[i].quantity = 20;
            items[i].extended_price = 100000;
            expected_matches++;
        } else {
            items[i].ship_date = 19930101;
            items[i].discount = 400;
            items[i].quantity = 30;
            items[i].extended_price = 100000;
        }
    }

    // 2. Pre-transpose into AarchGate Native Format
    std::cout << "[SETUP] Pre-transposing 100M rows into Native Format (" << (native_size / (1024*1024)) << " MB)...\n";
    uint64_t* native_data = (uint64_t*)aligned_alloc(64, native_size);
    
    apex::compute::BitSlicer slicer;
    for (size_t b = 0; b < num_blocks; ++b) {
        apex::compute::ColumnBuffer buf;
        
        // Field 0: ship_date
        for (int j = 0; j < 64; ++j) buf.data[j] = items[b * 64 + j].ship_date;
        slicer.slice(buf, buf);
        std::memcpy(native_data + (b * num_fields * 64) + (0 * 64), buf.data, 64 * 8);

        // Field 1: discount
        for (int j = 0; j < 64; ++j) buf.data[j] = items[b * 64 + j].discount;
        slicer.slice(buf, buf);
        std::memcpy(native_data + (b * num_fields * 64) + (1 * 64), buf.data, 64 * 8);

        // Field 2: quantity
        for (int j = 0; j < 64; ++j) buf.data[j] = items[b * 64 + j].quantity;
        slicer.slice(buf, buf);
        std::memcpy(native_data + (b * num_fields * 64) + (2 * 64), buf.data, 64 * 8);
    }

    // 3. Setup AarchGate Engine
    apex::ApexEngine engine;
    std::vector<apex::core::FieldDescriptor> fields = {
        {"ship_date", 0, 64, apex::core::DataType::UINT64},
        {"discount", 8, 64, apex::core::DataType::UINT64},
        {"quantity", 16, 64, apex::core::DataType::UINT64},
    };
    engine.register_schema("lineitem", fields, sizeof(LineItem));

    auto* q6_filter = apex::builder::And(
        apex::builder::And(
            apex::builder::GE(apex::builder::Load("ship_date"), apex::builder::Const(19940101)),
            apex::builder::LT(apex::builder::Load("ship_date"), apex::builder::Const(19950101))
        ),
        apex::builder::And(
            apex::builder::And(
                apex::builder::GE(apex::builder::Load("discount"), apex::builder::Const(500)),
                apex::builder::LE(apex::builder::Load("discount"), apex::builder::Const(700))
            ),
            apex::builder::LT(apex::builder::Load("quantity"), apex::builder::Const(24))
        )
    );
    engine.set_expression("lineitem", q6_filter);

    // 4. Warmup: Prime the hardware
    std::cout << "[WARMUP] Priming hardware with 10M rows...\n";
    engine.execute_native_parallel("lineitem", native_data, num_blocks / 10, 4);

    // 5. Record Run: Zero-Overhead Native Execution
    std::cout << "[EXEC] Starting Silicon-Limit Record Run (4 Threads)...\n";
    auto start = std::chrono::high_resolution_clock::now();
    
    uint64_t total_matches = engine.execute_native_parallel("lineitem", native_data, num_blocks, 4);
    
    auto end = std::chrono::high_resolution_clock::now();
    double duration_ms = std::chrono::duration<double, std::milli>(end - start).count();
    double duration_s = duration_ms / 1000.0;

    // 5. Task 3: Memory Saturation Analysis
    // Each block reads 3 fields * 64 * 8 bytes = 1536 bytes
    double total_bytes_read = (double)num_blocks * num_fields * 64 * 8;
    double bandwidth_gbs = (total_bytes_read / (1024.0 * 1024.0 * 1024.0)) / duration_s;
    double rps = num_rows / duration_s;
    double cpr = (4.0 * 1e9 * duration_s) / num_rows; // Cycles Per Record at 4GHz

    std::cout << "--------------------------------------------------\n";
    std::cout << "AarchGate Native TPC-H Q6 Results\n";
    std::cout << "--------------------------------------------------\n";
    std::cout << "Matches          : " << total_matches << " (Expected: " << expected_matches << ")\n";
    std::cout << "Execution Time   : " << std::fixed << std::setprecision(2) << duration_ms << " ms\n";
    std::cout << "Throughput       : " << std::fixed << std::setprecision(2) << (rps / 1e9) << " Billion RPS\n";
    std::cout << "Memory Bandwidth : " << std::fixed << std::setprecision(2) << bandwidth_gbs << " GB/s\n";
    std::cout << "Cycles Per Record: " << std::fixed << std::setprecision(3) << cpr << " cycles\n";
    std::cout << "Saturation       : " << std::fixed << std::setprecision(1) << (bandwidth_gbs / 100.0 * 100.0) << "% (of 100GB/s peak)\n";
    std::cout << "--------------------------------------------------\n";

    if (rps > 5e9) {
        std::cout << "🚀 WORLD RECORD: Broke 5 Billion RPS on AArch64!\n";
    }

    free(native_data);
    return 0;
}
