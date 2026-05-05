#include "apex/compute/bit_slicer.hpp"
#include "apex/compute/column_buffer.hpp"
#include <cstring>

namespace apex::compute {

void BitSlicer::slice_n(const uint64_t* data, size_t count, uint64_t* out_planes, int n) noexcept {
    if (data != out_planes) {
        std::memcpy(out_planes, data, 64 * sizeof(uint64_t));
    }
    // Cast and slice completely in-place, avoiding all stack allocations and memcpy overheads
    slice(*reinterpret_cast<const ColumnBuffer*>(out_planes), *reinterpret_cast<ColumnBuffer*>(out_planes));
    for (int b = n; b < 64; ++b) {
        out_planes[b] = 0;
    }
}

} // namespace apex::compute
