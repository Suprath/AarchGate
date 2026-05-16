# Benchmark Analysis

This document provides a detailed interpretation of the raw benchmark results obtained on the Apple Silicon M3 platform.

## 1. Bit-Slicer Throughput
The `bench_slicer.cpp` measures the raw speed of the 6-stage Knuth butterfly network.
- **Goal**: Verify sub-100ns latency per 64x64 block.
- **Results**: See `slicer_results.txt`.
- **Interpretation**: AarchGate consistently achieves ~80ns latency per block, which corresponds to 51.2 Gbps/core. This is sufficient to handle the bandwidth of 4 separate 10GbE network interfaces on a single core.

## 2. JIT Logic Performance
The `bench_jit_logic.cpp` compares the JIT-compiled ripple-carry circuits against standard C++ scalar loops.
- **Goal**: Prove the elimination of the Branching Tax.
- **Results**: See `jit_results.txt`.
- **Interpretation**: The JIT kernel outperforms scalar code by **10-15x** for simple predicates and up to **29x** for complex ML-style decision trees. This speedup is attributed to the elimination of pipeline flushes (branch mispredictions) and the increased Instruction-Level Parallelism (ILP).

## 3. Hardware Saturation
Comparison of the JIT throughput with the theoretical memory bus limits confirms that AarchGate operates in the **Memory-Bound** regime. This means the CPU is faster than the RAM, which is the ultimate goal of high-performance systems engineering.
