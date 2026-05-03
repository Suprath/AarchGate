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
    };

    /// Execute the JIT kernel across `total_rows` using `num_threads` worker threads.
    /// Each thread gets its own BitSlicer, ColumnBuffers, and scratchpad — zero shared state.
    /// Returns the total match count (sum of popcnt across all chunks).
    static uint64_t run(const void* data_ptr,
                        size_t total_rows,
                        const TaskConfig& config,
                        int num_threads = 4) noexcept;
};

} // namespace apex::compute
