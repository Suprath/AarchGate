#pragma once

#include "apex/core/types.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "apex/common.hpp"

namespace apex::core {

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const {
        return std::hash<std::string_view>()(sv);
    }
};

struct StringEqual {
    using is_transparent = void;
    bool operator()(std::string_view lhs, std::string_view rhs) const {
        return lhs == rhs;
    }
};

class SchemaRegistry {
public:
    SchemaRegistry() = default;
    ~SchemaRegistry() = default;

    void register_schema(std::string_view schema_name, std::vector<FieldDescriptor> fields);

    const FieldDescriptor* get_field(std::string_view schema_name, std::string_view field_name) const;

    bool has_schema(std::string_view schema_name) const;

private:
    using FieldMap = std::unordered_map<std::string, FieldDescriptor, StringHash, StringEqual>;
    std::unordered_map<std::string, FieldMap, StringHash, StringEqual> schemas_;
};

}
