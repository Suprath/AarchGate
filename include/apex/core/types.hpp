#pragma once

#include <cstdint>
#include <string>

namespace apex::core {

enum class DataType : uint8_t {
    UINT64 = 0,
    INT64 = 1,
    UINT32 = 2,
    INT32 = 3,
    FLOAT64 = 4,
};

struct alignas(64) FieldDescriptor {
    std::string name;
    uint32_t offset;
    uint32_t bit_width;
    DataType type;

    FieldDescriptor() = default;
    FieldDescriptor(std::string n, uint32_t off, uint32_t bits, DataType dt)
        : name(std::move(n)), offset(off), bit_width(bits), type(dt) {}
};

}
