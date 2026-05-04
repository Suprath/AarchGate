#include "apex/engine.hpp"
#include "apex/jit/ir.hpp"
#include <iostream>
#include <vector>
#include <sys/mman.h>
#include <cstring>

using namespace apex;

struct MarketTick {
    uint64_t ask;
    uint64_t bid;
    uint64_t spread;
    uint64_t prev_spread;
    uint64_t volume;
};

int main() {
    std::cout << "=== Simple GT Comparison Test ===\n" << std::flush;

    size_t num_rows = 100000;
    size_t alloc_size = num_rows * sizeof(MarketTick);

    std::cout << "Allocating " << (alloc_size / (1024)) << " KB...\n" << std::flush;

    void* memory = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) {
        std::cerr << "Memory allocation failed\n" << std::flush;
        return 1;
    }

    MarketTick* ticks = static_cast<MarketTick*>(memory);

    std::cout << "Initializing dataset...\n" << std::flush;
    for (size_t i = 0; i < num_rows; ++i) {
        ticks[i].ask = 10000 + i;
        ticks[i].bid = 9998;
        ticks[i].spread = 10;
        ticks[i].prev_spread = 5;
        ticks[i].volume = 2000;
    }

    ApexEngine engine;

    std::vector<core::FieldDescriptor> fields = {
        {"ask", offsetof(MarketTick, ask), 64, core::DataType::UINT64},
        {"bid", offsetof(MarketTick, bid), 64, core::DataType::UINT64},
        {"spread", offsetof(MarketTick, spread), 64, core::DataType::UINT64},
        {"prev_spread", offsetof(MarketTick, prev_spread), 64, core::DataType::UINT64},
        {"volume", offsetof(MarketTick, volume), 64, core::DataType::UINT64},
    };

    engine.register_schema("market", fields, sizeof(MarketTick));

    // Simple GT without AND
    auto* expr_root = builder::GT(builder::Load("ask"), builder::Load("bid"));

    std::cout << "Setting expression...\n" << std::flush;
    engine.set_expression("market", expr_root);
    std::cout << "Expression set.\n" << std::flush;

    std::cout << "Running execute...\n" << std::flush;
    uint64_t matches = engine.execute(ticks, num_rows);
    std::cout << "Matches: " << matches << "\n" << std::flush;

    munmap(memory, alloc_size);
    return 0;
}
