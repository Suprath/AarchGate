#include "apex/engine.hpp"
#include "apex/jit/ir.hpp"
#include <iostream>
#include <vector>
#include <sys/mman.h>

using namespace apex;

struct MarketTick {
    uint64_t ask;
    uint64_t bid;
    uint64_t spread;
    uint64_t prev_spread;
    uint64_t volume;
};

int main() {
    std::cout << "=== AND Expression Test ===\n" << std::flush;

    size_t num_rows = 100000;
    size_t alloc_size = num_rows * sizeof(MarketTick);

    void* memory = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) {
        return 1;
    }

    MarketTick* ticks = static_cast<MarketTick*>(memory);

    for (size_t i = 0; i < num_rows; ++i) {
        ticks[i].ask = 10000;
        ticks[i].bid = 9998;
        ticks[i].spread = (i %2 == 0) ? 10 : 5;
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

    // AND expression with GT, GT, GT
    auto* expr_root = builder::And(
        builder::And(
            builder::GT(builder::Load("ask"), builder::Const(9000)),
            builder::GT(builder::Load("spread"), builder::Load("prev_spread"))
        ),
        builder::GT(builder::Load("volume"), builder::Const(1000))
    );

    std::cout << "Setting expression...\n" << std::flush;
    engine.set_expression("market", expr_root);
    std::cout << "Expression set.\n" << std::flush;

    std::cout << "Running execute...\n" << std::flush;
    uint64_t matches = engine.execute(ticks, num_rows);
    std::cout << "Matches: " << matches << "\n" << std::flush;

    munmap(memory, alloc_size);
    return 0;
}
