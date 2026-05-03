#include "apex/engine.hpp"
#include "apex/jit/ir.hpp"
#include "apex/common.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>

using namespace apex;

struct MarketTick {
    uint64_t ask;
    uint64_t bid;
    uint64_t spread;
    uint64_t prev_spread;
    uint64_t volume;
};

int main() {
    std::cout << "=== Hybrid SDK Verification Test ===\n";

    size_t num_rows = 128ULL * 1000ULL; // 128k rows for fast test
    size_t alloc_size = num_rows * sizeof(MarketTick);

    MarketTick* ticks = static_cast<MarketTick*>(malloc(alloc_size));
    if (!ticks) {
        std::cerr << "Memory allocation failed.\n";
        return 1;
    }

    std::cout << "Initializing dataset...\n";
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

    ApexEngine engine_bit_sliced;
    ApexEngine engine_scalar;

    std::vector<core::FieldDescriptor> fields = {
        {"ask", offsetof(MarketTick, ask), 64, core::DataType::UINT64},
        {"bid", offsetof(MarketTick, bid), 64, core::DataType::UINT64},
        {"spread", offsetof(MarketTick, spread), 64, core::DataType::UINT64},
        {"prev_spread", offsetof(MarketTick, prev_spread), 64, core::DataType::UINT64},
        {"volume", offsetof(MarketTick, volume), 64, core::DataType::UINT64},
    };

    engine_bit_sliced.register_schema("market", fields, sizeof(MarketTick));
    engine_scalar.register_schema("market", fields, sizeof(MarketTick));

    auto build_expr = []() {
        return builder::And(
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
    };

    engine_bit_sliced.set_expression("market", build_expr(), ExecutionMode::BIT_SLICED);
    engine_scalar.set_expression("market", build_expr(), ExecutionMode::SCALAR);

    // Warmup
    engine_bit_sliced.execute_parallel(ticks, 1024, 4);
    engine_scalar.execute_parallel(ticks, 1024, 4);

    std::cout << "\nStarting tests...\n";

    // Bit-Sliced
    auto start_bs = std::chrono::high_resolution_clock::now();
    uint64_t bs_matches = engine_bit_sliced.execute_parallel(ticks, num_rows, 4);
    auto end_bs = std::chrono::high_resolution_clock::now();
    double bs_ms = std::chrono::duration<double, std::milli>(end_bs - start_bs).count();

    // Scalar
    auto start_sc = std::chrono::high_resolution_clock::now();
    uint64_t sc_matches = engine_scalar.execute_parallel(ticks, num_rows, 4);
    auto end_sc = std::chrono::high_resolution_clock::now();
    double sc_ms = std::chrono::duration<double, std::milli>(end_sc - start_sc).count();

    std::cout << "--------------------------------------------------\n";
    std::cout << "BIT_SLICED Mode (Complex Expression)\n";
    std::cout << "--------------------------------------------------\n";
    std::cout << "Matches : " << bs_matches << "\n";
    std::cout << "Time    : " << bs_ms << " ms\n";

    std::cout << "\n--------------------------------------------------\n";
    std::cout << "SCALAR Mode (Complex Expression)\n";
    std::cout << "--------------------------------------------------\n";
    std::cout << "Matches : " << sc_matches << "\n";
    std::cout << "Time    : " << sc_ms << " ms\n";

    if (bs_matches == sc_matches) {
        std::cout << "\n[PASS] BIT_SLICED and SCALAR matches are identical (" << bs_matches << " matches).\n";
    } else {
        std::cout << "\n[ERROR] Match mismatch! BIT_SLICED=" << bs_matches << ", SCALAR=" << sc_matches << "\n";
    }

    free(ticks);
    return 0;
}
