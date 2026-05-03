#include "apex/engine.hpp"
#include "apex/jit/ir.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <sys/mman.h>

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

// Scalar reference implementation
bool arbitrage_signal_reference(const MarketTick& tick) {
    int64_t diff = static_cast<int64_t>(tick.ask) - static_cast<int64_t>(tick.bid);
    return (diff < 5) && (tick.spread > tick.prev_spread) && (tick.volume > 1000);
}

int main() {
    std::cout << "=== 10B+ TPS Optimization Benchmark ===\n";
    
    // 128 Million rows
    size_t num_rows = 128ULL * 1000ULL * 1000ULL;
    size_t alloc_size = num_rows * sizeof(MarketTick);
    
    std::cout << "Allocating " << (alloc_size / (1024*1024)) << " MB using Superpages...\n";
    
#ifdef __APPLE__
    int fd = VM_MAKE_TAG(255) | VM_FLAGS_SUPERPAGE_SIZE_2MB;
    void* memory = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, fd, 0);
    if (memory == MAP_FAILED) {
        std::cout << "Superpage allocation failed, falling back to standard mmap.\n";
        memory = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }
#else
    void* memory = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_HUGETLB, -1, 0);
    if (memory == MAP_FAILED) {
        memory = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }
#endif

    if (memory == MAP_FAILED) {
        std::cerr << "Memory allocation failed.\n";
        return 1;
    }
    
    MarketTick* ticks = static_cast<MarketTick*>(memory);
    
    std::cout << "Initializing dataset...\n";
    // Initialize a subset to be true, rest false
    for (size_t i = 0; i < num_rows; ++i) {
        if (i % 100 == 0) { // 1% matches
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
    
    ApexEngine engine;
    
    std::vector<core::FieldDescriptor> fields = {
        {"ask", offsetof(MarketTick, ask), 64, core::DataType::UINT64},
        {"bid", offsetof(MarketTick, bid), 64, core::DataType::UINT64},
        {"spread", offsetof(MarketTick, spread), 64, core::DataType::UINT64},
        {"prev_spread", offsetof(MarketTick, prev_spread), 64, core::DataType::UINT64},
        {"volume", offsetof(MarketTick, volume), 64, core::DataType::UINT64},
    };

    engine.register_schema("market", fields, sizeof(MarketTick));
    
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
    
    engine.set_expression("market", expr_root);

    // Warmup
    engine.execute(ticks, 1024);
    engine.execute_parallel(ticks, 1024, 4);

    std::cout << "\nStarting benchmarks...\n";
    
    // Scalar baseline (Single thread)
    auto start_scalar = std::chrono::high_resolution_clock::now();
    uint64_t scalar_matches = engine.execute(ticks, num_rows);
    auto end_scalar = std::chrono::high_resolution_clock::now();
    double scalar_ms = std::chrono::duration<double, std::milli>(end_scalar - start_scalar).count();
    
    // Parallel (4 threads)
    auto start_parallel = std::chrono::high_resolution_clock::now();
    uint64_t parallel_matches = engine.execute_parallel(ticks, num_rows, 4);
    auto end_parallel = std::chrono::high_resolution_clock::now();
    double parallel_ms = std::chrono::duration<double, std::milli>(end_parallel - start_parallel).count();
    
    double scalar_tps = (num_rows / scalar_ms) * 1000.0;
    double parallel_tps = (num_rows / parallel_ms) * 1000.0;
    
    double scalar_ns_per_block = (scalar_ms * 1000000.0) / (num_rows / 64.0);
    double parallel_ns_per_block = (parallel_ms * 1000000.0) / (num_rows / 64.0);

    std::cout << "--------------------------------------------------\n";
    std::cout << "Scalar Baseline (1 Thread)\n";
    std::cout << "--------------------------------------------------\n";
    std::cout << "Matches : " << scalar_matches << "\n";
    std::cout << "Time    : " << scalar_ms << " ms\n";
    std::cout << "TPS     : " << std::fixed << std::setprecision(0) << scalar_tps << " rows/sec\n";
    std::cout << "Latency : " << std::fixed << std::setprecision(2) << scalar_ns_per_block << " ns / 64-row block\n\n";

    std::cout << "--------------------------------------------------\n";
    std::cout << "Parallel Optimized (4 Threads + SIMD Gather)\n";
    std::cout << "--------------------------------------------------\n";
    std::cout << "Matches : " << parallel_matches << "\n";
    std::cout << "Time    : " << parallel_ms << " ms\n";
    std::cout << "TPS     : " << std::fixed << std::setprecision(0) << parallel_tps << " rows/sec\n";
    std::cout << "Latency : " << std::fixed << std::setprecision(2) << parallel_ns_per_block << " ns / 64-row block\n\n";

    std::cout << "Speedup : " << std::fixed << std::setprecision(2) << (scalar_ms / parallel_ms) << "x\n";
    
    if (scalar_matches != parallel_matches) {
        std::cout << "\n[ERROR] Match counts differ!\n";
    } else {
        std::cout << "\n[PASS] Match counts verified identical.\n";
    }

    munmap(memory, alloc_size);
    return 0;
}
