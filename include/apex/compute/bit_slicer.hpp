#pragma once

#include "apex/common.hpp"
#include "apex/compute/column_buffer.hpp"
#include <cstddef>
#include <cstdint>

namespace apex::compute {

class APEX_API BitSlicer {
public:
    void slice(const ColumnBuffer& in, ColumnBuffer& out) noexcept;

    /// Slice only the first N bits of the 64-bit input values.
    void slice_n(const uint64_t* data, size_t count, const uint64_t*& out_planes, int n) noexcept;
};

} // namespace apex::compute
