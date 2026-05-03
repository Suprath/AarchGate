#include "apex/AarchGate.hpp"
#include "apex/jit/ir.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>

using namespace apex;

struct alignas(64) GenericRecord {
    uint64_t field0;
    uint64_t field1;
    uint64_t field2;
    uint64_t padding[5];
};

int main() {
    std::cout << "=== Universal C++ SDK Verification Test ===" << std::endl;
    
    ApexEngine engine;

    std::vector<core::FieldDescriptor> fields = {
        {"Field0", (uint32_t)offsetof(GenericRecord, field0), 64, core::DataType::UINT64},
        {"Field1", (uint32_t)offsetof(GenericRecord, field1), 64, core::DataType::UINT64},
        {"Field2", (uint32_t)offsetof(GenericRecord, field2), 64, core::DataType::UINT64}
    };
    engine.register_schema("GenericSchema", fields, sizeof(GenericRecord));

    // Logic: (Field0 + Field1) > Field2
    auto f0 = builder::Load("Field0");
    auto f1 = builder::Load("Field1");
    auto sum = builder::Add(f0, f1);
    auto f2 = builder::Load("Field2");
    auto root = builder::GT(sum, f2);

    engine.set_expression("GenericSchema", root, ExecutionMode::SCALAR);

    const size_t num_rows = 1000000;
    std::vector<GenericRecord> dataset(num_rows);
    
    // Generate data
    for (size_t i = 0; i < num_rows; ++i) {
        dataset[i].field0 = i;
        dataset[i].field1 = 1000;
        dataset[i].field2 = (i % 2 == 0) ? (i + 500) : (i + 2000);
    }

    auto start = std::chrono::high_resolution_clock::now();
    uint64_t matches = engine.execute(dataset.data(), num_rows);
    auto end = std::chrono::high_resolution_clock::now();

    std::cout << "Standard Stride (64-byte) Matches: " << matches << std::endl;
    
    // --- TASK 3: MIXED STRIDE STRESS TEST (21 bytes) ---
    std::cout << "\n--- Mixed Stride Stress Test (21 bytes) ---" << std::endl;
    
    // Define a 21-byte record: 8 bytes (uint64) + 1 byte (uint8) + 8 bytes (uint64) + 4 bytes padding
    const size_t odd_stride = 21;
    std::vector<uint8_t> odd_dataset(num_rows * odd_stride);
    
    std::vector<core::FieldDescriptor> odd_fields = {
        {"Odd0", 0, 64, core::DataType::UINT64},   // Offset 0
        {"Odd1", 9, 64, core::DataType::UINT64}    // Offset 9 (Un-aligned!)
    };
    engine.register_schema("OddSchema", odd_fields, odd_stride);
    
    auto logic_odd = builder::GT(builder::Load("Odd0"), builder::Load("Odd1"));
    engine.set_expression("OddSchema", logic_odd, ExecutionMode::BIT_SLICED);

    for (size_t i = 0; i < num_rows; ++i) {
        uint8_t* row = &odd_dataset[i * odd_stride];
        uint64_t val0 = i;
        uint64_t val1 = 500000;
        std::memcpy(row + 0, &val0, 8);
        std::memcpy(row + 9, &val1, 8);
    }

    auto start_odd = std::chrono::high_resolution_clock::now();
    uint64_t matches_odd = engine.execute(odd_dataset.data(), num_rows);
    auto end_odd = std::chrono::high_resolution_clock::now();

    std::cout << "Mixed Stride Matches: " << matches_odd << " (Expected: 499999)" << std::endl;
    std::cout << "Mixed Stride Time: " << std::chrono::duration<double, std::milli>(end_odd - start_odd).count() << " ms" << std::endl;

    if (matches == 500000 && matches_odd == 499999) {
        std::cout << "[PASS] Universal C++ SDK Test (including Mixed Stride)" << std::endl;
        return 0;
    } else {
        std::cout << "[FAIL] Incorrect matches" << std::endl;
        return 1;
    }
}
