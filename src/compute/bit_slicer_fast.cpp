#include "apex/compute/bit_slicer.hpp"
#include "apex/compute/column_buffer.hpp"
#include <cstring>

namespace apex::compute {

void BitSlicer::slice_n(const uint64_t* data, size_t count, uint64_t* out_planes, int n) noexcept {
    // Optimized partial slice for N bits. 
    // We use a temporary buffer because the engine calls this in-place (data == out_planes),
    // and writing directly to out_planes would overwrite data mid-transpose.
    uint64_t temp[64];
    for (int b = 0; b < n; ++b) {
        uint64_t mask = 1ULL << b;
        uint64_t res = 0;
        for (size_t i = 0; i < 64; ++i) {
            if (data[i] & mask) res |= (1ULL << i);
        }
        temp[b] = res;
    }
    // Zero-out the remaining planes to ensure the JIT kernel doesn't pick up garbage
    for (int b = n; b < 64; ++b) {
        temp[b] = 0;
    }
    std::memcpy(out_planes, temp, 64 * sizeof(uint64_t));
}

} // namespace apex::compute
