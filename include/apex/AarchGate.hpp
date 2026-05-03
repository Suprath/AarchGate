#pragma once

#include "apex/apex_c_api.h"
#include <string>
#include <vector>
#include <stdexcept>

namespace apex {

class Apex {
public:
    Apex() {
        handle_ = apex_create();
        if (!handle_) {
            throw std::runtime_error("Failed to create Apex Engine");
        }
    }

    ~Apex() {
        if (handle_) {
            apex_destroy(handle_);
        }
    }

    // Prevent copying
    Apex(const Apex&) = delete;
    Apex& operator=(const Apex&) = delete;

    void register_schema(const std::string& name, const std::vector<apex_field_descriptor_t>& fields, size_t stride) {
        if (apex_register_schema(handle_, name.c_str(), fields.data(), fields.size(), stride) != 0) {
            throw std::runtime_error("Failed to register schema");
        }
    }

    void set_logic(const std::string& schema_name, void* ir_root_ptr, int mode = APEX_EXEC_MODE_BIT_SLICED) {
        if (apex_set_logic(handle_, schema_name.c_str(), ir_root_ptr, mode) != 0) {
            throw std::runtime_error("Failed to set logic");
        }
    }

    uint64_t execute(const void* data_ptr, size_t count) {
        uint64_t result = apex_execute(handle_, data_ptr, count);
        if (result == (uint64_t)-1) {
            throw std::runtime_error("Execution failed");
        }
        return result;
    }

private:
    apex_engine_h handle_;
};

} // namespace apex
