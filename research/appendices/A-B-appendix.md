# Appendix A: Methodology

This appendix details the experimental environment and measurement strategies used to validate the performance claims made in this paper.

## A.1 Hardware Specifications

All experiments were performed on a dedicated Apple Mac Studio workstation to ensure thermal stability and consistent power delivery.

*   **Processor**: Apple M3 Ultra
    *   16 Performance Cores (P-Cores) @ 4.05 GHz
    *   8 Efficiency Cores (E-Cores) @ 2.8 GHz
    *   L1D Cache: 128 KB per P-Core
    *   L2 Cache: 16 MB shared per cluster
*   **Memory**: 128 GB LPDDR5x-6400 (Unified)
    *   Peak Bandwidth: 800 GB/s

## A.2 Software Environment

*   **OS**: macOS Sonoma 14.5 (Kernel: Darwin 23.5.0)
*   **Compiler**: Apple Clang 15.0.0
    *   Flags: `-O3 -ffast-math -mcpu=apple-m3 -std=c++20`
*   **Libraries**:
    *   AsmJit v1.11.0
    *   Google Highway v1.0.7
    *   iceoryx v2.0.0
    *   simdjson v3.9.0

## A.3 Measurement Strategy

Throughput and latency measurements were conducted using the following protocol:
1.  **Warmup**: Every test was preceded by 1,000,000 "cold" iterations to bring the CPU and memory controller into a steady state and ensure the JIT cache was populated.
2.  **Repetitions**: Tests were repeated 1,000 times, and the median value was used to minimize the impact of OS jitter.
3.  **Clock Source**: Timing was measured using `std::chrono::high_resolution_clock`, which on macOS utilizes the ARM64 `CNTPCT_EL0` physical counter with nanosecond precision.
4.  **Error Handling**: Standard deviation and p99 latencies were calculated for all datasets to verify consistency.

# Appendix B: Source Code Listings

This appendix provides curated code listings of the critical execution paths in AarchGate.

## B.1 ARM64 Ripple-Carry Logic Emission

The following C++ code from `jit/compiler.cpp` demonstrates the emission of the branchless comparison circuit using AsmJit.

```cpp
// Emit logic for one bit-plane (A_i) where Constant_bit is 0
void emit_comparison_step_0(a64::Assembler& a, Reg gt, Reg eq, Reg plane) {
    a.and_(x12, eq, plane);   // temp = EQ & A_i
    a.orr(gt, gt, x12);       // GT |= temp
    a.bic(eq, eq, plane);     // EQ &= ~A_i
}

// Emit logic for one bit-plane (A_i) where Constant_bit is 1
void emit_comparison_step_1(a64::Assembler& a, Reg eq, Reg plane) {
    a.and_(eq, eq, plane);    // EQ &= A_i
}
```

## B.2 6-Stage Butterfly Transpose (Google Highway)

The following snippet from `compute/bit_slicer.cpp` shows the vectorized stage logic for the 64x64 transposition.

```cpp
// Stage 5: 32-bit stride swap
template <class D, class V>
void TransposeStage5(D d, V& a, V& b) {
    const auto m = Set(d, 0x00000000FFFFFFFFULL);
    auto mask = And(Xor(ShiftRight<32>(a), b), m);
    a = Xor(a, ShiftLeft<32>(mask));
    b = Xor(b, mask);
}
```
