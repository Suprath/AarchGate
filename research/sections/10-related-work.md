# 10. Related Work

The development of AarchGate builds upon several decades of research in vectorized query execution, bit-sliced indexing, and JIT compilation.

## 10.1 Vectorization and Bit-Slicing

The concept of bit-sliced indexing was pioneered by **Johnson** [#Johnson1999] and later refined by **Lemire et al.** [#Lemire2015] for compressed bitmap structures like Roaring Bitmaps. While these works focused on static storage, AarchGate extends the paradigm to **runtime transposition** of live data streams.

The use of SIMD for database scans was extensively explored by **Willhalm et al.** [#Willhalm2013] and **Polychroniou et al.** [#Polychroniou2015]. These works demonstrated the potential of wide vector registers for predicate evaluation. AarchGate advances this field by introducing the 6-stage Knuth butterfly for near-zero-cost transposition on ARM64 and by synthesizing branchless ripple-carry circuits via JIT, rather than using static SIMD intrinsics.

## 10.2 JIT Compilation in Databases

The transition from interpreted query engines to JIT-compiled engines was significantly influenced by the **HyPer** project and the work of **Thomas Neumann** [#Neumann2011]. HyPer demonstrated that compiling query plans into machine code can eliminate interpretation overhead. AarchGate adopts this philosophy but applies it specifically to **bit-sliced logic synthesis**, whereas HyPer and its successors (e.g., Umbra) primarily focus on row-wise or columnar-vectorized machine code.

## 10.3 Analytical Engines

Modern columnar engines like **DuckDB** [#Raasveldt2019] and **ClickHouse** utilize vectorized execution to amortize the cost of interpretation over batches of data. While these systems achieve high performance, they still rely on traditional row-wise logic for many operations. AarchGate-Eureka demonstrates that by combining bit-sliced execution with two-pass deferred materialization, it is possible to achieve an order-of-magnitude improvement in scan throughput compared to these state-of-the-art columnar engines.

## 10.4 Machine Learning Inference

In the ML domain, **Apache TVM** [#Chen2018] and **TensorRT** focus on optimizing deep learning graphs. However, for tabular data dominated by Gradient-Boosted Decision Trees (GBDTs), inference is traditionally branch-heavy. AarchGate-ML introduces a novel approach by flattening trees into logic gates, a technique that complements the graph-level optimizations of TVM by addressing the microarchitectural branch penalty in GBDT evaluation.

# 11. Conclusion

AarchGate represents a fundamental advancement in the design of high-throughput execution engines for ARM64 architectures. By transforming row-oriented data into parallel bit-planes and synthesizing branchless, ripple-carry machine code, AarchGate eliminates the transcoding and branching taxes that bottleneck traditional systems.

Our evaluation has shown that AarchGate consistently operates at the physical limits of the silicon, achieving record-breaking performance in both machine learning inference (**207M rows/s**) and log analytics (**61 GB/s**). Furthermore, the deterministic nature of bit-sliced execution provides a 110x improvement in energy efficiency compared to traditional branch-heavy engines.

As ARM64 continues to dominate the data center and edge computing landscapes, execution primitives like AarchGate will be essential for managing the ever-increasing volume of global data. Future work will explore the integration of **SVE2 (Scalable Vector Extensions)** for wider logic gates and the expansion of the bit-sliced model to complex join operations and multi-dimensional spatial queries.
