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
                       ExecutionMode mode = ExecutionMode::BIT_SLICED);

    uint64_t execute(const void* data_ptr, size_t row_count);
    uint64_t execute_parallel(const void* data_ptr, size_t row_count, int num_threads = -1) noexcept;
    std::vector<double> execute_vector(const void* data_ptr, size_t row_count, double precision_multiplier);

    // Native Bit-Sliced Path: Zero-Overhead Silicon Limit
    // bit_planes: contiguous block of [num_blocks * num_fields * 64] uint64s
    // num_blocks: number of 64-row blocks to process
    uint64_t execute_native(std::string_view schema_name, const uint64_t* bit_planes, size_t num_blocks) noexcept;

    uint64_t execute_native_parallel(std::string_view schema_name, const uint64_t* bit_planes, size_t num_blocks, int num_threads = -1) noexcept;

    // Concurrency Control
    void set_thread_count(int count) noexcept { num_threads_ = count; }
    int get_thread_count() const noexcept { return num_threads_; }

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
        ir::ResultKind result_kind;
        
        // --- Hybrid Popcount Aggregation ---
        uint64_t base_sum = 0;
        std::vector<int64_t> delta_weights;
        std::vector<int> masks_to_popcount;
        int active_bits = 64;
    };

    core::SchemaRegistry registry_;
    jit::JitCompiler compiler_;
    compute::BitSlicer slicer_;
    
    std::unordered_map<std::string, SchemaMetadata> schema_metadata_;
    std::unordered_map<std::string, CompiledLogic> compiled_logic_;
    std::unordered_map<std::string, ExprCompiledLogic> expr_logic_;

    // Performance buffers (pre-allocated to avoid heap churn)
    mutable std::array<compute::ColumnBuffer, 128> field_buffers_;
    mutable const uint64_t* field_planes_array_[128];

    int num_threads_;

    uint64_t process_chunk_expr(const void* data_ptr,
                               size_t row_stride,
                               size_t row_count,
                               const ExprCompiledLogic& expr_logic,
                               uint64_t* scratchpad) noexcept;
};

} // namespace apex
