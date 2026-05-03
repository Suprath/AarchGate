// (c) 2024-2026 Suprath PS. All rights reserved.
// Project Apex: Universal JIT-Accelerated Vector Engine (10B+ RPS)
//
// This work is licensed under the Business Source License 1.1 until 2029-05-03,
// transitioning to the Apache License 2.0 thereafter.
// See the LICENSE file in the repository root for the full text.

#include "apex/core/registry.hpp"
#include <vector>
#include <stdexcept>
#include <iostream>

namespace apex::core {

void SchemaRegistry::register_schema(std::string_view schema_name, std::vector<FieldDescriptor> fields) {
    FieldMap field_map;
    for (auto& field : fields) {
        field_map.emplace(field.name, std::move(field));
    }
    schemas_.emplace(std::string(schema_name), std::move(field_map));
}

const FieldDescriptor* SchemaRegistry::get_field(std::string_view schema_name, std::string_view field_name) const {
    auto schema_it = schemas_.find(schema_name);
    if (schema_it == schemas_.end()) {
        return nullptr;
    }

    auto field_it = schema_it->second.find(field_name);
    if (field_it != schema_it->second.end()) {
        return &field_it->second;
    }

    return nullptr;
}

bool SchemaRegistry::has_schema(std::string_view schema_name) const {
    return schemas_.count(schema_name) > 0;
}

}