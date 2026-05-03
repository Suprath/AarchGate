#pragma once

#include "apex/core/registry.hpp"
#include <cstdint>
#include <cstring>
#include <string_view>

namespace apex::memory {

class DataViewer {
public:
    DataViewer(const void* base, const core::SchemaRegistry& registry) noexcept
        : base_(static_cast<const uint8_t*>(base)), registry_(registry)
    {}

    // Branchless pointer-cast read
    template <typename T>
    T get_value(std::string_view schema, std::string_view field) const noexcept {
        const auto* desc = registry_.get_field(schema, field);
        T result{};
        __builtin_memcpy(&result, base_ + desc->offset, sizeof(T));
        return result;
    }

private:
    const uint8_t* base_;
    const core::SchemaRegistry& registry_;
};

} // namespace apex::memory
