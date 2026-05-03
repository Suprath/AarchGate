#include "apex/core/registry.hpp"
#include "apex/core/types.hpp"
#include <cassert>
#include <iostream>

using namespace apex::core;

int main() {
    SchemaRegistry registry;

    std::vector<FieldDescriptor> market_tick_fields{
        {"timestamp", 0, 64, DataType::UINT64},
        {"symbol_id", 8, 32, DataType::UINT32},
        {"padding", 12, 32, DataType::UINT32},
        {"bid", 16, 64, DataType::UINT64},
        {"ask", 24, 64, DataType::UINT64},
        {"volume", 32, 64, DataType::UINT64},
    };

    registry.register_schema("MarketTick", market_tick_fields);

    assert(registry.has_schema("MarketTick"));

    const auto* bid_field = registry.get_field("MarketTick", "bid");
    assert(bid_field != nullptr);
    assert(bid_field->name == "bid");
    assert(bid_field->offset == 16);
    assert(bid_field->bit_width == 64);
    assert(bid_field->type == DataType::UINT64);

    const auto* timestamp_field = registry.get_field("MarketTick", "timestamp");
    assert(timestamp_field != nullptr);
    assert(timestamp_field->offset == 0);

    const auto* nonexistent_field = registry.get_field("MarketTick", "nonexistent");
    assert(nonexistent_field == nullptr);

    const auto* nonexistent_schema = registry.get_field("NonexistentSchema", "bid");
    assert(nonexistent_schema == nullptr);

    std::cout << "✓ All registry tests passed!" << std::endl;
    return 0;
}