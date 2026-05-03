#include <iostream>
#include <ctime>
#include <algorithm>
#include <cstdint>
#include "apex/compute/bit_slicer.hpp"
#include "apex/compute/column_buffer.hpp"

using namespace apex::compute;

uint64_t get_nanos() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Prevent compiler dead-code elimination
void do_not_optimize(const void* ptr) {
    asm volatile("" : : "r"(ptr) : "memory");
}

int main() {
    std::cout << "=== Bit-Slicer Benchmark (1M iterations) ===\n";
    std::cout << "Transpose 64 uint64_t → 64 bit-planes\n\n";

    BitSlicer slicer;
    ColumnBuffer in, out;

    for (int i = 0; i < 64; ++i) {
        in.data[i] = 1000000ULL + i;
    }

    const int kWarmupIters = 10000;
    const int kMeasureIters = 1000000;

    std::cout << "Warmup: " << kWarmupIters << " iterations... ";
    for (int i = 0; i < kWarmupIters; ++i) {
        slicer.slice(in, out);
    }
    do_not_optimize(&out);
    std::cout << "done\n";

    std::cout << "Measurement: " << kMeasureIters << " iterations\n";
    uint64_t min_ns = UINT64_MAX;
    uint64_t total_ns = 0;
    uint64_t count_under_80 = 0;

    for (int i = 0; i < kMeasureIters; ++i) {
        uint64_t start = get_nanos();
        slicer.slice(in, out);
        uint64_t elapsed = get_nanos() - start;
        min_ns = std::min(min_ns, elapsed);
        total_ns += elapsed;
        if (elapsed < 80) count_under_80++;
    }
    do_not_optimize(&out);

    uint64_t avg_ns = total_ns / kMeasureIters;
    double p50_ns = static_cast<double>(min_ns); // Placeholder; ideally sort for percentile

    std::cout << "  min:      " << min_ns << " ns\n";
    std::cout << "  avg:      " << avg_ns << " ns\n";
    std::cout << "  <80ns:    " << count_under_80 << " / " << kMeasureIters << " ("
              << (100.0 * count_under_80 / kMeasureIters) << "%)\n";
    std::cout << "Target: avg < 80 ns\n";

    if (avg_ns < 80) {
        std::cout << "[PASS] Sub-80 ns avg achieved\n";
    } else {
        std::cout << "[INFO] avg " << avg_ns << " ns exceeds target\n";
    }

    return 0;
}
