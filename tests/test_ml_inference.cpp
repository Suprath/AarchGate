// (c) 2024-2026 Suprath PS. All rights reserved.
// Project Apex: Universal JIT-Accelerated Vector Engine (10B+ RPS)
//
// Verification Test: ML Inference (Phase 1)
// Goal: Validate Decision Forest logic (SELECT/ADD) at scale.

#include "apex/AarchGate.hpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <iomanip>

using namespace apex;
using namespace apex::builder;

struct RowData {
    uint64_t field0; // Feature A
    uint64_t field1; // Feature B
};

int main() {
    std::cout << "=== AarchGate ML Inference Verification (Phase 1) ===" << std::endl;

    ApexEngine engine;

    // Define schema using FieldDescriptors
    std::vector<core::FieldDescriptor> fields;
    fields.emplace_back("Field0", 0, 64, core::DataType::UINT64);
    fields.emplace_back("Field1", 8, 64, core::DataType::UINT64);

    // Register schema (Field0 at offset 0, Field1 at offset 8, Total stride 16 bytes)
    engine.register_schema("ml_schema", fields, 16);

    // Model:
    // Tree1: (Field0 > 100) ? 50 : 10
    // Tree2: (Field1 < 50) ? 20 : 5
    // Result = Tree1 + Tree2

    auto* feature_a = Load("Field0");
    auto* feature_b = Load("Field1");

    auto* tree1 = Select(GT(feature_a, Const(100)), Const(50), Const(10));
    auto* tree2 = Select(LT(feature_b, Const(50)), Const(20), Const(5));

    // Combine the trees (Decision Forest)
    auto* model = Add(tree1, tree2);

    // Use set_expression instead of compile_expression
    engine.set_expression("ml_schema", model);

    // Generate 1 million rows
    const size_t num_rows = 1000000;
    std::vector<RowData> data(num_rows);
    uint64_t expected_matches = 0;

    for (size_t i = 0; i < num_rows; ++i) {
        data[i].field0 = (i % 200); // 0-199
        data[i].field1 = (i % 100); // 0-99

        // Scalar reference:
        // Tree1: (Field0 > 100) ? 50 : 10
        uint64_t v1 = (data[i].field0 > 100) ? 50 : 10;
        // Tree2: (Field1 < 50) ? 20 : 5
        uint64_t v2 = (data[i].field1 < 50) ? 20 : 5;

        expected_matches += (v1 + v2);
    }

    uint64_t actual_result = engine.execute(data.data(), num_rows);

    std::cout << "Rows processed: " << num_rows << std::endl;
    std::cout << "Expected result (Sum of Forest): " << expected_matches << std::endl;
    std::cout << "Actual result (JIT): " << actual_result << std::endl;

    if (actual_result == expected_matches) {
        std::cout << "VERIFICATION SUCCESSFUL" << std::endl;
        return 0;
    } else {
        std::cout << "VERIFICATION FAILED" << std::endl;
        return 1;
    }
}
