// (c) 2024-2026 Suprath PS. All rights reserved.
// Project Apex: Universal JIT-Accelerated Vector Engine (10B+ RPS)
//
// This work is licensed under the Business Source License 1.1 until 2029-05-03,
// transitioning to the Apache License 2.0 thereafter.
// See the LICENSE file in the repository root for the full text.

#include "apex/jit/ir.hpp"
#include <cstring>

// Global thread-local node pool
thread_local apex::builder::NodePool apex::builder::g_pool;

namespace apex {
namespace builder {

ir::Node* Load(std::string_view field_name) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::LOAD;
    n->result_kind = ir::ResultKind::BITPLANE;
    std::strncpy(n->field_name, field_name.data(), sizeof(n->field_name) - 1);
    return n;
}

ir::Node* Const(int64_t value) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::CONST;
    n->result_kind = ir::ResultKind::BITPLANE;
    n->const_value = value;
    return n;
}

ir::Node* Add(ir::Node* a, ir::Node* b) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::ADD;
    n->result_kind = ir::ResultKind::BITPLANE;
    n->left = a;
    n->right = b;
    return n;
}

ir::Node* Sub(ir::Node* a, ir::Node* b) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::SUB;
    n->result_kind = ir::ResultKind::BITPLANE;
    n->left = a;
    n->right = b;
    return n;
}

ir::Node* GT(ir::Node* a, ir::Node* b) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::GT;
    n->result_kind = ir::ResultKind::BITMASK;
    n->left = a;
    n->right = b;
    return n;
}

ir::Node* LT(ir::Node* a, ir::Node* b) noexcept {
    // LT(a, b) = GT(b, a)
    return GT(b, a);
}

ir::Node* EQ(ir::Node* a, ir::Node* b) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::EQ;
    n->result_kind = ir::ResultKind::BITMASK;
    n->left = a;
    n->right = b;
    return n;
}

ir::Node* And(ir::Node* a, ir::Node* b) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::AND;
    n->result_kind = ir::ResultKind::BITMASK;
    n->left = a;
    n->right = b;
    return n;
}

ir::Node* Or(ir::Node* a, ir::Node* b) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::OR;
    n->result_kind = ir::ResultKind::BITMASK;
    n->left = a;
    n->right = b;
    return n;
}

ir::Node* Not(ir::Node* a) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::NOT;
    n->result_kind = ir::ResultKind::BITMASK;
    n->left = a;
    return n;
}

ir::Node* Select(ir::Node* cond, ir::Node* a, ir::Node* b) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::SELECT;
    n->result_kind = ir::ResultKind::BITMASK;
    n->cond = cond;
    n->left = a;
    n->right = b;
    return n;
}

ir::Node* LSL(ir::Node* a, int shift) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::LSL;
    n->result_kind = ir::ResultKind::BITPLANE;
    n->left = a;
    n->shift_amount = shift;
    return n;
}

ir::Node* LSR(ir::Node* a, int shift) noexcept {
    ir::Node* n = g_pool.alloc();
    if (!n) return nullptr;
    n->kind = ir::NodeKind::LSR;
    n->result_kind = ir::ResultKind::BITPLANE;
    n->left = a;
    n->shift_amount = shift;
    return n;
}

} // namespace builder
} // namespace apex

namespace apex::jit {

void initialize_compiler() {
    // Initialize AsmJit-based JIT compiler
}

} // namespace apex::jit
