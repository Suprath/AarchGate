#pragma once

#include <asmjit/a64.h>
#include <cstdint>
#include <memory>
#include <string_view>

#include "apex/common.hpp"

namespace apex {
namespace ir {
    struct Node;
}
namespace core {
    class SchemaRegistry;
}

namespace jit {

using KernelFunc = uint64_t (*)(const uint64_t* bit_planes);
using ExprKernelFunc = uint64_t (*)(const uint64_t* const* field_planes, uint64_t* scratch);

class APEX_API JitCompiler {
public:
    JitCompiler() noexcept;
    ~JitCompiler() noexcept;

    KernelFunc compile_comparison(uint64_t threshold) noexcept;

    ExprKernelFunc compile_expression(
        ir::Node* root,
        const core::SchemaRegistry& registry,
        std::string_view schema_name) noexcept;

    ExprKernelFunc compile_scalar_expression(
        ir::Node* root,
        const core::SchemaRegistry& registry,
        std::string_view schema_name) noexcept;

private:
    void dump_bytecode(const asmjit::CodeHolder& code, const char* label) const noexcept;

    std::unique_ptr<asmjit::JitRuntime> runtime_;
};

} // namespace jit
} // namespace apex
