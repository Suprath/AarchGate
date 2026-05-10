# Project Apex: Ultra-Low Latency Development Guide

**Project Apex** is a 10B+ TPS Universal JIT Logic Engine designed for extreme performance in sub-microsecond latencies.

## ULL (Ultra-Low Latency) Directives

These rules are **non-negotiable** for all code paths in hot loops and data paths:

### 1. Zero Heap
- **Rule**: No dynamic allocation in the hot path.
- **Rationale**: Heap allocation is unpredictable; memory fragmentation and GC pauses kill latency.
- **Practice**:
  - Use stack allocation and fixed-size buffers (arrays, std::array).
  - Pre-allocate pools at initialization (not in transaction/message processing loops).
  - Use arena allocators or thread-local buffers for temporary objects.
  - Document which functions are "hot-path safe" (zero-alloc).

### 2. Branchless
- **Rule**: Use bitwise logic and conditional move (cmov) instead of branches.
- **Rationale**: CPU branch misprediction flushes the pipeline; branchless code maintains throughput.
- **Practice**:
  - Prefer `(cond ? val1 : val2)` ternary or bitwise select: `(mask & a) | (~mask & b)`.
  - Use lookup tables (LUTs) instead of if-else chains.
  - Avoid loops with data-dependent exit conditions; unroll or use SIMD.
  - Let the compiler (-O3 -march=native) transform branches into cmov where possible.

### 3. Cache-First
- **Rule**: Align hot structs to 64 bytes (L1D cache line on modern x86/ARM).
- **Rationale**: False sharing and cache line bouncing cause silent performance cliffs.
- **Practice**:
  - Apply `alignas(64)` to struct definitions used in tight loops.
  - Keep frequently-accessed fields in the first cache line.
  - Separate hot and cold data (e.g., metadata in a different cache line).
  - Use `-march=native` to expose SIMD; structure data for SIMD loads.

### 4. Fixed-Point Arithmetic
- **Rule**: Use `int64_t` (or `uint64_t`) for all numeric values; avoid floating-point in latency-critical paths.
- **Rationale**: Floating-point operations are slower and non-deterministic across platforms.
- **Practice**:
  - Represent decimals/fixed-point values as integers (e.g., nanoseconds, price in basis points).
  - Define clear units in comments (e.g., `// units: nanoseconds`).
  - Use saturating arithmetic or overflow checks where semantically required.
  - Prefer bitwise shifts over division/multiplication where applicable.

## Compiler Flags

All builds use aggressive optimization for production:

```bash
-O3                    # Maximum optimization
-march=native          # CPU-specific instructions (SIMD, etc.)
-ffast-math            # Unsafe floating-point optimizations (if needed)
-fno-finite-math-only  # Allow inf/nan (safety net; override -ffast-math)
```

## Code Organization

- **src/core**: Fundamental primitives, memory pools, locks.
- **src/jit**: JIT compiler and code generation.
- **src/compute**: Logic execution engine, bytecode interpreter.
- **src/api**: Public API and FFI boundaries.
- **src/memory**: Memory management, arenas, custom allocators.
- **src/telemetry**: Metrics, tracing, observability (kept out of hot paths).
- **include/apex**: Public headers.
- **fbs**: FlatBuffers schemas (.fbs files).
- **benchmarks**: Performance microbenchmarks.
- **tests**: Unit and integration tests.

## Testing & Validation

- Unit tests in `tests/` with a focus on correctness.
- Benchmark microbenchmarks in `benchmarks/` to validate latency targets.
- Use compiler sanitizers in debug builds (ASan, UBSan, TSan) to catch subtle bugs early.
- Profile with `perf`, `cachegrind`, or CPU sampling to identify bottlenecks.

## Dependencies

- **AsmJit**: Runtime code generation (x86/x64, ARM).
- **Google Highway**: Portable SIMD library.
- **Apple Metal**: Low-level GPGPU compute framework (macOS platform).
- **iceoryx**: Zero-copy IPC and pub-sub transport.
- **FlatBuffers**: Schema-driven, zero-copy serialization.

## Performance Targets

- **Throughput**: 1.3B+ CPU RPS / 10B+ GPU Transactions Per Second.
- **Latency**: Sub-microsecond p99 (< 1000 ns) per 64-row vector batch.
- **Memory**: Fixed-size, page-aligned (4096-byte) allocations for zero-copy CPU-to-GPU handoffs.
- **Schema Scale**: Up to 128 fields per schema (fully unrolled bit-plane transposition limits).

---

**Golden Rule**: If code touches the hot path, it must be measurable (via benchmarks) and optimized (via profiling).
