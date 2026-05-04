#include "apex/engine.hpp"
#include "apex/jit/ir.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <sys/mman.h>
#include <cstring>

#ifdef __APPLE__
#include <mach/vm_statistics.h>
#ifndef VM_FLAGS_SUPERPAGE_SIZE_2MB
#define VM_FLAGS_SUPERPAGE_SIZE_2MB (2 << 16)
#endif
#endif

using namespace apex;

struct MarketTick {
    uint64_t ask;
    uint64_t bid;
    uint64_t spread;
    uint64_t prev_spread;
    uint64_t volume;
};

bool arbitrage_signal_reference(const MarketTick& tick) {
    int64_t diff = static_cast<int64_t>(tick.ask) - static_cast<int64_t>(tick.bid);
    return (diff < 5) && (tick.spread > tick.prev_spread) && (tick.volume > 1000);
}

int main() {
    std::cout << "=== 10B+ TPS Optimization Benchmark (DEBUG) ===\n" << std::flush;

    // Start with 1 million rows instead of 128 million
    size_t num_rows = 1ULL * 1000ULL * 1000ULL;
    size_t alloc_size = num_rows * sizeof(MarketTick);

    std::cout << "Allocating " << (alloc_size / (1024*1024)) << " MB...\n" << std::flush;

#ifdef __APPLE__
    int fd = VM_MAKE_TAG(255) | VM_FLAGS_SUPERPAGE_SIZE_2MB;
    void* memory = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, fd, 0);
    if (memory == MAP_FAILED) {
        std::cout << "Superpage allocation failed, falling back to standard mmap.\n" << std::flush;
        memory = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }
#else
    void* memory = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_HUGETLB, -1, 0);
    if (memory == MAP_FAILED) {
        std::cout << "Huge pages allocation failed, falling back to standard mmap.\n" << std::flush;
        memory = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }
#endif

    if (memory == MAP_FAILED) {
        std::cerr << "Memory allocation failed: " << strerror(errno) << "\n" << std::flush;
        return 1;
    }

    std::cout << "Memory allocated at " << memory << "\n" << std::flush;

    MarketTick* ticks = static_cast<MarketTick*>(memory);

    std::cout << "Initializing dataset...\n" << std::flush;
    for (size_t i = 0; i < num_rows; ++i) {
        if (i % 100 == 0) {
            ticks[i].ask = 10000;
            ticks[i].bid = 9998;
            ticks[i].spread = 10;
            ticks[i].prev_spread = 5;
            ticks[i].volume = 2000;
        } else {
            ticks[i].ask = 15000;
            ticks[i].bid = 14998;
            ticks[i].spread = 10;
            ticks[i].prev_spread = 15;
            ticks[i].volume = 500;
        }
    }
    std::cout << "Dataset initialized.\n" << std::flush;

    std::cout << "Creating engine...\n" << std::flush;
    ApexEngine engine;

    std::vector<core::FieldDescriptor> fields = {
        {"ask", offsetof(MarketTick, ask), 64, core::DataType::UINT64},
        {"bid", offsetof(MarketTick, bid), 64, core::DataType::UINT64},
        {"spread", offsetof(MarketTick, spread), 64, core::DataType::UINT64},
        {"prev_spread", offsetof(MarketTick, prev_spread), 64, core::DataType::UINT64},
        {"volume", offsetof(MarketTick, volume), 64, core::DataType::UINT64},
    };

    std::cout << "Registering schema...\n" << std::flush;
    engine.register_schema("market", fields, sizeof(MarketTick));

    std::cout << "Building expression...\n" << std::flush;
    auto* expr_root = builder::And(
        builder::And(
            builder::LT(
                builder::Sub(
                    builder::Load("ask"),
                    builder::Load("bid")
                ),
                builder::Const(5)
            ),
            builder::GT(
                builder::Load("spread"),
                builder::Load("prev_spread")
            )
        ),
        builder::GT(
            builder::Load("volume"),
            builder::Const(1000)
        )
    );

    std::cout << "Setting expression...\n" << std::flush;
    engine.set_expression("market", expr_root);
    std::cout << "Expression set.\n" << std::flush;

    std::cout << "Running warmup...\n" << std::flush;
    engine.execute(ticks, 1024);
    engine.execute_parallel(ticks, 1024, 4);
    std::cout << "Warmup done.\n" << std::flush;

    std::cout << "Starting scalar execution...\n" << std::flush;
    auto start_scalar = std::chrono::high_resolution_clock::now();
    uint64_t scalar_matches = engine.execute(ticks, num_rows);
    auto end_scalar = std::chrono::high_resolution_clock::now();
    double scalar_ms = std::chrono::duration<double, std::milli>(end_scalar - start_scalar).count();

    std::cout << "Scalar done: " << scalar_matches << " matches in " << scalar_ms << "ms\n" << std::flush;

    std::cout << "Cleaning up...\n" << std::flush;
    munmap(memory, alloc_size);
    std::cout << "Done.\n" << std::flush;
    return 0;
}
