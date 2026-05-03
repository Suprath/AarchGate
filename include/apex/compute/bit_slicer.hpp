#pragma once

#include "apex/common.hpp"
#include "apex/compute/column_buffer.hpp"

namespace apex::compute {

class APEX_API BitSlicer {
public:
    void slice(const ColumnBuffer& in, ColumnBuffer& out) noexcept;
};

} // namespace apex::compute
