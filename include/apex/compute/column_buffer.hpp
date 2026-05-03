#pragma once

#include <cstdint>

namespace apex::compute {

struct alignas(64) ColumnBuffer {
    static constexpr int kSize = 64;
    uint64_t data[kSize];
};

} // namespace apex::compute
