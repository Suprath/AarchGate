#include "apex/compute/bit_slicer.hpp"
#include "apex/compute/column_buffer.hpp"
#include <cstring>

namespace apex::compute {

void BitSlicer::slice_n(const uint64_t* data, size_t count, const uint64_t*& out_planes, int n) noexcept {
    // Optimized partial slice for N bits. 
    // We reuse the input ColumnBuffer's data area (64 uint64s) as the destination.
    // Transpose 64xN subset.
    uint64_t* out = const_cast<uint64_t*>(out_planes);
    for (int b = 0; b < n; ++b) {
        uint64_t mask = 1ULL << b;
        uint64_t res = 0;
        for (size_t i = 0; i < 64; ++i) {
            if (data[i] & mask) res |= (1ULL << i);
        }
        out[b] = res;
    }
}

} // namespace apex::compute
