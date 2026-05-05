#include "apex/compute/bit_slicer.hpp"
#include "apex/compute/column_buffer.hpp"
#include <cstring>

namespace apex::compute {

void BitSlicer::slice_n(const uint64_t* data, size_t count, uint64_t* out_planes, int n) noexcept {
    ColumnBuffer temp_in;
    std::memcpy(temp_in.data, data, 64 * sizeof(uint64_t));
    ColumnBuffer temp_out;
    slice(temp_in, temp_out);
    for (int b = n; b < 64; ++b) {
        temp_out.data[b] = 0;
    }
    std::memcpy(out_planes, temp_out.data, 64 * sizeof(uint64_t));
}

} // namespace apex::compute
