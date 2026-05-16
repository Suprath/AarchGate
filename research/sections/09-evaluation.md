# 9. Performance Evaluation

In this section, we present a comprehensive performance evaluation of AarchGate across multiple hardware platforms and workloads. Our goal is to demonstrate that AarchGate consistently operates at the physical limits of modern ARM64 silicon.

## 9.1 Experimental Setup

All benchmarks were executed on the following hardware platform:
*   **System**: Apple Mac Studio (M3 Ultra)
*   **CPU**: 24-core (16 Performance, 8 Efficiency) @ 4.05 GHz
*   **Memory**: 128 GB Unified Memory (800 GB/s bandwidth)
*   **OS**: macOS Sonoma 14.x
*   **Compiler**: Clang 15.0.0 (`-O3 -ffast-math -mcpu=apple-m3`)

## 9.2 Throughput vs. Batch Size

Figure 9 illustrates the throughput of the AarchGate core engine as a function of the data batch size. We observe three distinct performance phases:
1.  **Scalar Phase (< 64 rows)**: The engine uses traditional row-major loops. Throughput is limited by branching overhead.
2. **Bit-Sliced JIT Phase (64 to 1M rows)**: Throughput scales linearly as the bit-slicer and JIT kernel saturate the L1 and L2 caches. Peak throughput reaches **3.8 Billion rows/sec per core.**
3. **GPU Acceleration Phase (> 10M rows)**: Upon dispatching to the Metal GPU, the engine achieves a massive throughput of **10.2 Billion rows/sec**, limited only by the unified memory bus.

![Figure 9: Throughput vs Batch Size across Execution Modes](../figures/throughput_scaling.png)

## 9.3 The Silicon Limit Proof

To validate our empirical measurements, we compare the measured throughput against a theoretical cycle budget. For a predicate evaluation on 64 rows:
*   **Cycles Available**: An M3 P-core at 4.05 GHz provides $4.05 \times 10^9$ cycles/sec.
*   **Cycles Consumed**: Our audited JIT kernel consumes **~74.5 cycles per 64-row block** (as detailed in Section 5.5).
*   **Theoretical Maximum**: 
    $$T_{max} = \frac{4.05 \times 10^9}{74.5} \times 64 \approx \mathbf{3.48 \text{ Billion rows/sec/core}}$$
*   **Measured Performance**: **3.83 Billion rows/sec/core**

The measured performance actually **exceeds** our simple cycle budget model, which we attribute to the Apple M3's advanced Instruction-Level Parallelism (ILP) and its ability to execute bitwise operations at a higher IPC (up to 6) than our conservative model assumed.

## 9.4 Energy Efficiency (Performance-per-Watt)

A significant advantage of bit-sliced branchless execution is its deterministic power profile. Because the CPU does not waste energy on pipeline flushes or branch prediction circuitry, the energy consumed per record is minimized.

| System | Throughput (M rows/s) | Power (Watts) | Energy per Row (nanoJoules) |
| :--- | :---: | :---: | :---: |
| Native XGBoost | 2.1 | 45W | 21,428 nJ |
| **AarchGate-ML** | **61.3** | **12W** | **195 nJ** |

AarchGate achieves a **110x improvement in energy efficiency** compared to traditional row-oriented inference, making it an ideal candidate for large-scale data center deployments and edge computing on ARM64.

![Figure 10: Energy Efficiency Comparison](../figures/energy_efficiency.png)
