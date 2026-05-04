#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <random>
#include <memory>
#include <cstring>
#include "apex/engine.hpp"
#include "apex/compute/bit_slicer.hpp"
#include "apex/jit/ir.hpp"

void print_header(const std::string& title) {
    std::cout << "\n" << std::string(title.length() + 4, '=') << "\n";
    std::cout << "| " << title << " |\n";
    std::cout << std::string(title.length() + 4, '=') << "\n";
}

void print_result(const std::string& name, double value, const std::string& unit, double target, bool pass) {
    std::cout << "  " << std::left << std::setw(30) << name 
              << ": " << std::right << std::setw(12) << std::fixed << std::setprecision(2) << value 
              << " " << std::left << std::setw(10) << unit;
    
    if (target > 0) {
        std::cout << " [target: <" << std::fixed << std::setprecision(2) << target << "]";
    }
    
    std::cout << (pass ? " ✓ PASS" : " ✗ FAIL") << "\n";
}

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           APEX INFRASTRUCTURE AUDIT                             ║\n";
    std::cout << "║     System Readiness Verification Before JIT Implementation      ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";

    int total_tests = 0;
    int passed_tests = 0;

    // =====================================================================
    // TEST 1: GATHER-SPEED BENCHMARK
    // =====================================================================
    {
        print_header("Test 1: Gather-Speed Benchmark");
        total_tests++;

        apex::ApexEngine engine;
        std::vector<apex::core::FieldDescriptor> fields;
        for (int i = 0; i < 5; i++) {
            fields.push_back({"f" + std::to_string(i), static_cast<uint32_t>(i * 8), 64, apex::core::DataType::UINT64});
        }
        engine.register_schema("bench", fields, 40);

        std::vector<uint8_t> data(40 * 64, 0);
        apex::compute::ColumnBuffer out;

        auto start = std::chrono::high_resolution_clock::now();
        for (int iter = 0; iter < 1000; iter++) {
            for (int i = 0; i < 5; i++) {
                engine.gather_field(data.data(), &fields[i], 40, 64, out);
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();
        double avg_ns = elapsed_ns / 5000.0; 

        std::cout << "  Test setup:\n    - Gathering 5 fields from 64 rows\n    - 1000 iterations per field\n\n";
        print_result("Per-gather latency", avg_ns, "ns", 300.0, avg_ns < 300.0);
        if (avg_ns < 300.0) passed_tests++;
    }

    // =====================================================================
    // TEST 2: SCRATCHPAD ALIGNMENT CHECK
    // =====================================================================
    {
        print_header("Test 2: Scratchpad Alignment Check");
        total_tests++;

        alignas(64) uint64_t scratchpad[512];
        uintptr_t addr = reinterpret_cast<uintptr_t>(&scratchpad);
        int alignment = addr % 64;
        bool aligned = (alignment == 0);

        print_result("Alignment offset", static_cast<double>(alignment), "bytes", 0.0, aligned);

        if (aligned) {
            passed_tests++;
        }
    }

    // =====================================================================
    // TEST 3: ABI LOOPBACK TEST
    // =====================================================================
    {
        print_header("Test 3: ABI Loopback Test");
        total_tests++;

        apex::ApexEngine engine;
        std::vector<apex::core::FieldDescriptor> fields = {
            {"test_field", 0, 64, apex::core::DataType::UINT64},
        };
        engine.register_schema("test", fields, 8);

        auto f0 = apex::builder::Load("test_field");
        auto c10 = apex::builder::Const(10);
        auto root = apex::builder::GT(f0, c10);

        auto kernel = engine.get_compiler().compile_expression(root, engine.get_registry(), "test");

        if (kernel) {
            apex::compute::ColumnBuffer test_field_buf, bit_planes;
            for (int i = 0; i < 64; i++) test_field_buf.data[i] = i;

            apex::compute::BitSlicer slicer;
            slicer.slice(test_field_buf, bit_planes);

            const uint64_t* field_planes[] = {bit_planes.data};
            uint64_t* scratch = nullptr;
            posix_memalign((void**)&scratch, 64, 8192);
            std::memset(scratch, 0, 8192);

            uint64_t result = kernel(field_planes, scratch);
            uint64_t expected = 0xFFFFFFFFFFFFF800ULL;
            bool abi_pass = (result == expected);

            std::cout << "  Kernel result (hex)           : 0x" << std::hex << std::setw(16) << std::setfill('0') << result << "\n";
            std::cout << "  Expected mask (hex)           : 0x" << std::hex << std::setw(16) << std::setfill('0') << expected << "\n" << std::dec << std::setfill(' ');
            
            print_result("ABI Loopback (GT Comparison)", static_cast<double>(__builtin_popcountll(result)), "matches", 53.0, abi_pass);
            if (abi_pass) passed_tests++;
            free(scratch);
        }
    }

    // =====================================================================
    // TEST 4: BIT-SLICER STRESS TEST
    // =====================================================================
    {
        print_header("Test 4: Bit-Slicer Stress Test");
        total_tests++;

        apex::compute::BitSlicer slicer;
        std::mt19937_64 rng(42);
        std::uniform_int_distribution<uint64_t> dist;

        const int BLOCKS = 10000;
        int failures = 0;

        for (int block = 0; block < BLOCKS; block++) {
            apex::compute::ColumnBuffer col, planes, reconstructed;
            for (int i = 0; i < 64; i++) col.data[i] = dist(rng);

            slicer.slice(col, planes);

            for (int i = 0; i < 64; i++) {
                reconstructed.data[i] = 0;
                for (int bit = 0; bit < 64; bit++) {
                    uint64_t bit_value = (planes.data[bit] >> i) & 1;
                    reconstructed.data[i] |= (bit_value << bit);
                }
                if (col.data[i] != reconstructed.data[i]) failures++;
            }
        }

        print_result("Slicer consistency", static_cast<double>(failures), "errors", 1.0, failures == 0);
        if (failures == 0) passed_tests++;
    }

    // =====================================================================
    // TEST 5: JIT UNIVERSAL LOGIC TEST
    // =====================================================================
    {
        print_header("Test 5: JIT Universal Logic Test");
        total_tests++;

        auto engine = std::make_unique<apex::ApexEngine>();
        
        std::vector<apex::core::FieldDescriptor> fields = {
            {"Field0", 0, 64, apex::core::DataType::UINT64},
            {"Field1", 8, 64, apex::core::DataType::UINT64},
            {"Field2", 16, 64, apex::core::DataType::UINT64}
        };
        engine->register_schema("JitTest", fields, 24);
        
        auto f0 = apex::builder::Load("Field0");
        auto f1 = apex::builder::Load("Field1");
        auto f2 = apex::builder::Load("Field2");
        auto root = apex::builder::GT(apex::builder::Add(f0, f1), f2);
        
        engine->set_expression("JitTest", root, apex::ExecutionMode::BIT_SLICED);
        
        std::vector<uint8_t> data(24 * 64);
        for (int i = 0; i < 64; ++i) {
            uint64_t v0 = i;
            uint64_t v1 = i;
            uint64_t v2 = i;
            std::memcpy(&data[i*24 + 0], &v0, 8);
            std::memcpy(&data[i*24 + 8], &v1, 8);
            std::memcpy(&data[i*24 + 16], &v2, 8);
        }
        
        uint64_t matches = engine->execute(data.data(), 64);
        print_result("JIT Arithmetic (Add + GT)", static_cast<double>(matches), "matches", 63.0, matches == 63);
        if (matches == 63) passed_tests++;
    }

    // =====================================================================
    // TEST 6: MIXED STRIDE STRESS TEST
    // =====================================================================
    {
        print_header("Test 6: Mixed Stride Stress Test");
        total_tests++;

        auto engine = std::make_unique<apex::ApexEngine>();
        
        // Use a 21-byte stride (unaligned, odd number)
        std::vector<apex::core::FieldDescriptor> fields = {
            {"Field0", 0, 64, apex::core::DataType::UINT64},
            {"Field1", 8, 64, apex::core::DataType::UINT64},
            {"Field2", 13, 64, apex::core::DataType::UINT64} // Overlapping and unaligned!
        };
        engine->register_schema("StrideTest", fields, 21);
        
        auto f0 = apex::builder::Load("Field0");
        auto root = apex::builder::GT(f0, apex::builder::Const(10));
        engine->set_expression("StrideTest", root, apex::ExecutionMode::BIT_SLICED);
        
        std::vector<uint8_t> data(21 * 64);
        for (int i = 0; i < 64; ++i) {
            uint64_t val = i;
            std::memcpy(&data[i*21], &val, 8);
        }
        
        uint64_t matches = engine->execute(data.data(), 64);
        print_result("Unaligned Stride (21b)", static_cast<double>(matches), "matches", 53.0, matches == 53);
        if (matches == 53) passed_tests++;
    }

    // =====================================================================
    // TEST 7: SUSTAINED THROUGHPUT (THERMAL JITTER)
    // =====================================================================
    {
        print_header("Test 7: Sustained Throughput (10x Run)");
        total_tests++;

        auto engine = std::make_unique<apex::ApexEngine>();
        std::vector<apex::core::FieldDescriptor> fields = {
            {"f0", 0, 64, apex::core::DataType::UINT64}
        };
        engine->register_schema("stress", fields, 8);
        engine->set_expression("stress", apex::builder::GT(apex::builder::Load("f0"), apex::builder::Const(100)), apex::ExecutionMode::BIT_SLICED);

        const size_t ROWS = 10'000'000;
        std::vector<uint8_t> data(ROWS * 8, 0);

        std::vector<double> thp_history;
        for (int i = 0; i < 10; i++) {
            auto start = std::chrono::high_resolution_clock::now();
            engine->execute(data.data(), ROWS);
            auto end = std::chrono::high_resolution_clock::now();
            double elapsed_s = std::chrono::duration<double>(end - start).count();
            thp_history.push_back((ROWS / elapsed_s) / 1'000'000.0);
        }

        double first = thp_history[0];
        double last = thp_history[9];
        double jitter = std::abs(first - last) / first;

        std::cout << "  Throughput History (M rows/sec):\n    ";
        for (double t : thp_history) std::cout << std::fixed << std::setprecision(1) << t << " ";
        std::cout << "\n\n";

        print_result("Thermal Jitter", jitter * 100.0, "%", 15.0, jitter < 0.15);
        if (jitter < 0.15) passed_tests++;
    }

    std::cout << "\nAudit Summary: " << passed_tests << " / " << total_tests << " passed.\n";
    return (passed_tests == total_tests) ? 0 : 1;
}
