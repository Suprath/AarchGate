#pragma once

#include "apex/compute/bit_slicer.hpp"
#include "apex/compute/column_buffer.hpp"
#include "apex/jit/compiler.hpp"
#include "apex/core/registry.hpp"
#include "apex/common.hpp"
#include <cstdint>
#include <vector>

namespace apex::compute {

class ParallelRunner {
public:
    struct TaskConfig {
        jit::ExprKernelFunc kernel;
        std::vector<const core::FieldDescriptor*> fields;
        size_t row_stride;
        ExecutionMode mode;
        int result_kind = 0;  // 0=BITPLANE (arithmetic), 1=BITMASK (boolean)

        // --- Hybrid Popcount Aggregation ---
        uint64_t base_sum = 0;
        std::vector<int64_t> delta_weights;
        std::vector<int> masks_to_popcount;
        int active_bits = 64; 
    };

    // Per-thread BITPLANE aggregation result with execution statistics
    struct ExecutionStats {
        uint64_t total_matches;    // Sum of bit-counts across all threads
        uint64_t total_cycles;     // Total execution cycles (sum of thread cycles)
        uint64_t avg_cycles_per_row;
    };

    /// Execute the JIT kernel across `total_rows` using `num_threads` worker threads.
    /// Each thread gets its own BitSlicer, ColumnBuffers, and scratchpad — zero shared state.
    /// Returns the total match count (sum of popcnt across all BITPLANE chunks).
    static uint64_t run(const void* data_ptr,
                        size_t total_rows,
                        const TaskConfig& config,
                        int num_threads = 4) noexcept;

    /// Execute with detailed statistics (cycles, per-thread metrics).
    static ExecutionStats run_with_stats(const void* data_ptr,
                                         size_t total_rows,
                                         const TaskConfig& config,
                                         int num_threads = 4) noexcept;
};

} // namespace apex::compute
