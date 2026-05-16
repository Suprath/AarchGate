# 6. GPGPU Accelerated Compute

While the ARM64 CPU cores provide ultra-low latency for small-to-medium datasets, massive parallel throughput is best achieved through GPGPU acceleration. AarchGate exploits the **Unified Memory Architecture (UMA)** of Apple Silicon to bridge the CPU and GPU without the traditional "PCIe Tax" of memory copying.

## 6.1 Unified Memory and Zero-Copy Bridge

On traditional x86 systems with discrete GPUs (e.g., NVIDIA/AMD), data must be copied from System RAM to Video RAM over the PCIe bus, introducing millisecond-scale latency. On Apple Silicon, the CPU and GPU share the same physical memory pool.

By utilizing the page-aligned allocation strategy discussed in Section 2, AarchGate allows the Metal GPU to "mount" the same physical buffers that the Bit-Slicer just populated. This enables a **Zero-Copy Handshake**, where the CPU transposes the data and the GPU immediately evaluates it in-place.

## 6.2 Dynamic MSL Transpilation

Just as the CPU JIT synthesizes ARM64 assembly, the AarchGate `ApexEngine` can dynamically generate **Metal Shading Language (MSL)** source code at runtime. This allows the engine to adapt the GPU kernel to the specific structure of a query or a machine learning model.

The transpiler converts AST nodes into high-performance C++14-based MSL. For simple bitwise filters, the transpilation is direct. However, for arithmetic operations (addition, subtraction), the GPU cannot use standard ripple-carry logic because the threads are isolated.

## 6.3 Kogge-Stone Parallel Prefix Scans

To perform bitwise arithmetic across a GPU SIMDgroup (32 threads), AarchGate implements the **Kogge-Stone Algorithm** [#Kogge1973]. Kogge-Stone is a parallel prefix scan that calculates carries in $O(log_2(N))$ time.

Instead of waiting for bit 0 to "ripple" to bit 63, the Kogge-Stone kernel uses **SIMD Shuffle** instructions (`simd_shuffle_up`) to "jump" carry bits across threads. This allows a 64-bit addition to be completed in just 6 shuffle-and-xor steps.

```cpp
// Listing 6.1: MSL Kogge-Stone Parallel Carry Scan
kernel void kogge_stone_add(device uint64_t* A, device uint64_t* B, uint tid [[thread_index_in_simdgroup]]) {
    uint64_t g = A[tid] & B[tid]; // Generate
    uint64_t p = A[tid] ^ B[tid]; // Propagate
    
    // Logarithmic Carry Jumps (Kogge-Stone)
    for (int offset = 1; offset < 32; offset <<= 1) {
        uint64_t p_prev = simd_shuffle_up(p, offset);
        uint64_t g_prev = simd_shuffle_up(g, offset);
        g |= (p & g_prev);
        p &= p_prev;
    }
    // Result calculation follows...
}
```

## 6.4 Throughput Scaling: CPU vs. GPU

The decision to dispatch to the GPU is governed by the **Throughput-to-Latency Tradeoff.**
*   **CPU (Bit-Sliced JIT)**: Best for batch sizes < 1 Million rows. Latency is sub-millisecond, but throughput is capped by core count.
*   **GPU (Metal Compute)**: Best for batch sizes > 10 Million rows. Incurs a "Startup Latency" for shader compilation and command buffer submission, but provides massive parallelism.

In our benchmarks, the M3 GPU achieves a sustained throughput of over **10 Billion Records per second** for simple logic evaluation, effectively saturating the unified memory bandwidth of the SoC.

![Figure 6: Kogge-Stone Parallel Carry Tree](../figures/kogge_stone.png)
