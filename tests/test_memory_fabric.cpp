#include <iostream>
#include <cassert>

#ifdef APEX_HAS_ICEORYX
#include "apex/memory/data_viewer.hpp"
#include "apex/core/registry.hpp"
#include "apex/core/types.hpp"

using namespace apex::memory;
using namespace apex::core;

struct alignas(64) UniversalRecord {
    uint64_t field0;
    uint32_t field1;
    uint32_t padding;
    uint64_t field2;
    uint64_t field3;
    uint64_t field4;
};
#endif

int main() {
    std::cout << "=== Module 2 Test: Memory Fabric ===\n\n";

#ifdef APEX_HAS_ICEORYX
    // Setup SchemaRegistry
    SchemaRegistry registry;
    std::vector<FieldDescriptor> universal_fields{
        {"field0", 0, 64, DataType::UINT64},
        {"field1", 8, 32, DataType::UINT32},
        {"padding", 12, 32, DataType::UINT32},
        {"field2", 16, 64, DataType::UINT64},
        {"field3", 24, 64, DataType::UINT64},
        {"field4", 32, 64, DataType::UINT64},
    };
    registry.register_schema("UniversalRecord", universal_fields);
    std::cout << "✓ UniversalRecord schema registered\n";

    // Create a UniversalRecord struct (stack-allocated for fallback testing)
    UniversalRecord record;
    record.field0 = 1000000;
    record.field1 = 42;
    record.padding = 0;
    record.field2 = 25000;
    record.field3 = 25100;
    record.field4 = 5000;

    const void* data_addr = &record;
    std::cout << "✓ Created UniversalRecord struct at addr " << data_addr << "\n";

    // Test DataViewer: extract fields from buffer using schema registry
    DataViewer viewer(data_addr, registry);

    uint64_t val2 = viewer.get_value<uint64_t>("UniversalRecord", "field2");
    assert(val2 == 25000 && "Field2 value mismatch");
    std::cout << "✓ DataViewer extracted field2: " << val2 << "\n";

    uint64_t val3 = viewer.get_value<uint64_t>("UniversalRecord", "field3");
    assert(val3 == 25100 && "Field3 value mismatch");
    std::cout << "✓ DataViewer extracted field3: " << val3 << "\n";

    uint64_t val0 = viewer.get_value<uint64_t>("UniversalRecord", "field0");
    assert(val0 == 1000000 && "Field0 mismatch");
    std::cout << "✓ DataViewer extracted field0: " << val0 << "\n";

    uint32_t val1 = viewer.get_value<uint32_t>("UniversalRecord", "field1");
    assert(val1 == 42 && "Field1 mismatch");
    std::cout << "✓ DataViewer extracted field1: " << val1 << "\n";

    std::cout << "\n✓ Memory fabric core functionality verified!\n";
    std::cout << "[Note: Full zero-copy IPC test with ShmFabric and Publisher/Subscriber\n";
    std::cout << " requires POSIX ACL support - see DEVELOPMENT_GUIDE.md for native testing]\n";
    return 0;

#else
    std::cout << "APEX_HAS_ICEORYX not defined; memory fabric tests skipped\n";
    return 1;
#endif
}
