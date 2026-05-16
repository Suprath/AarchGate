#include <iostream>
#include <chrono>
#include <vector>
#include <iomanip>
#include <random>
#include "../../include/apex/compute/bit_slicer.hpp"
#include "../../include/apex/compute/column_buffer.hpp"

/**
 * RESEARCH BENCHMARK 01: Transposition Substrate Throughput
 * Goal: Measure the raw performance of the Knuth 6-Stage Butterfly network.
 * Metrics: Throughput (Million 64x64 blocks/sec), Latency (ns per block).
 */

void run_slicer_benchmark() {
    apex::compute::BitSlicer slicer;
    apex::compute::ColumnBuffer in, out;
    
    // Seed with random data to prevent CPU pattern optimization
    std::mt19937_64 rng(42);
    for(int i=0; i<64; ++i) in.data[i] = rng();

    constexpr size_t iterations = 10'000'000;
    
    std::cout << "Starting Slicer Benchmark (" << iterations << " iterations)...\n";
    
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        slicer.slice(in, out);
        // Prevent compiler from eliding the loop
        if (out.data[0] == 0x1) in.data[0]++; 
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    std::chrono::duration<double> elapsed = end - start;
    double avg_ns = (elapsed.count() * 1e9) / iterations;
    double throughput_m_blocks = (iterations / elapsed.count()) / 1e6;

    std::cout << "--- Results ---\n";
    std::cout << "Total Time      : " << elapsed.count() << " s\n";
    std::cout << "Avg Latency     : " << std::fixed << std::setprecision(2) << avg_ns << " ns/block\n";
    std::cout << "Throughput      : " << throughput_m_blocks << " M blocks/sec\n";
    std::cout << "Rows per second : " << throughput_m_blocks * 64 << " M rows/sec\n";
}

int main() {
    run_slicer_benchmark();
    return 0;
}
