#pragma once

#include "apex/common.hpp"
#include "apex/core/registry.hpp"
#include "apex/core/types.hpp"
#include "apex/compute/bit_slicer.hpp"
#include "apex/compute/column_buffer.hpp"
#include "apex/jit/compiler.hpp"
#include "apex/jit/ir.hpp"
#include <string>
#include <string_view>
#include <memory>
#include <cstring>
#include <unordered_map>
#include <array>
#include <vector>

namespace apex {

class APEX_API ApexEngine {
public:
    ApexEngine() noexcept;
    ~ApexEngine() noexcept = default;

    void register_schema(std::string_view schema_name,
                        const std::vector<core::FieldDescriptor>& fields,
                        size_t total_row_stride) noexcept;

    void set_logic(std::string_view schema_name,
                   std::string_view field_name,
                   uint64_t threshold) noexcept;

    void set_expression(std::string_view schema_name, 
                       ir::Node* expr_root, 
                       ExecutionMode mode = ExecutionMode::BIT_SLICED) noexcept;

    uint64_t execute(const void* data_ptr, size_t row_count) noexcept;

    uint64_t execute_parallel(const void* data_ptr, size_t row_count, int num_threads = 4) noexcept;

    // Internal access for tests
    jit::JitCompiler& get_compiler() noexcept { return compiler_; }
    core::SchemaRegistry& get_registry() noexcept { return registry_; }
    
    // Performance critical helpers
    void gather_field(const void* data_ptr,
                     const core::FieldDescriptor* field,
                     size_t row_stride,
                     size_t row_count,
                     compute::ColumnBuffer& out) const noexcept;

private:
    struct SchemaMetadata {
        size_t row_stride;
        std::vector<core::FieldDescriptor> fields;
    };

    struct CompiledLogic {
        const core::FieldDescriptor* field;
        jit::KernelFunc kernel;
    };

    struct ExprCompiledLogic {
        jit::ExprKernelFunc kernel;
        std::vector<const core::FieldDescriptor*> fields; // Maps index to descriptor
        ExecutionMode mode;
    };

    core::SchemaRegistry registry_;
    jit::JitCompiler compiler_;
    compute::BitSlicer slicer_;
    
    std::unordered_map<std::string, SchemaMetadata> schema_metadata_;
    std::unordered_map<std::string, CompiledLogic> compiled_logic_;
    std::unordered_map<std::string, ExprCompiledLogic> expr_logic_;

    // Performance buffers (pre-allocated to avoid heap churn)
    mutable std::array<compute::ColumnBuffer, 8> field_buffers_;
    mutable const uint64_t* field_planes_array_[8];

    uint64_t process_chunk_expr(const void* data_ptr,
                               size_t row_stride,
                               size_t row_count,
                               const ExprCompiledLogic& expr_logic,
                               uint64_t* scratchpad) noexcept;
};

} // namespace apex
