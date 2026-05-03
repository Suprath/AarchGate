#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include "apex/common.hpp"

namespace apex {
namespace ir {

enum class NodeKind : uint8_t {
    LOAD,     // Load field from bit-planes
    CONST,    // Constant value
    ADD,      // Arithmetic add (ripple carry)
    SUB,      // Arithmetic sub (2's complement)
    GT,       // Greater-than comparison
    LT,       // Less-than comparison
    GE,       // Greater-than-or-equal comparison
    LE,       // Less-than-or-equal comparison
    EQ,       // Equal comparison
    AND,      // Logical AND of masks
    OR,       // Logical OR of masks
    NOT,      // Logical NOT of mask
    SELECT,   // Ternary SELECT/MUX: cond ? a : b
    LSL,      // Logical shift left (index remap only)
    LSR,      // Logical shift right (index remap only)
};

enum class ResultKind : uint8_t {
    BITPLANE, // Arithmetic result: 64 bit-planes, stored in scratchpad slot
    BITMASK,  // Logic/comparison result: single 64-bit mask, stored in scalar register
};

struct Node {
    NodeKind kind;
    ResultKind result_kind = ResultKind::BITPLANE;

    // Node content (union-like, interpreted per kind)
    char field_name[64] = {};     // For LOAD: field name (fixed size, no heap)
    int64_t const_value = 0;      // For CONST: the constant value
    int shift_amount = 0;         // For LSL/LSR: number of bits to shift

    // Tree structure
    Node* left = nullptr;         // Left operand
    Node* right = nullptr;        // Right operand
    Node* cond = nullptr;         // For SELECT: condition

    // Compile-time metadata (filled by JitCompiler during analysis)
    int slot_id = -1;             // Scratchpad slot ID (BITPLANE nodes only)
    int field_idx = -1;           // Field index in field_planes array (LOAD nodes)
    int carry_reg = -1;           // Which callee-saved register holds carry (x24+)
};

} // namespace ir

namespace builder {

// Global node pool for tree construction (setup-time only, no heap during execute)
struct NodePool {
    static constexpr int MAX_NODES = 128;
    ir::Node storage[MAX_NODES];
    int next_idx = 0;

    ir::Node* alloc() noexcept {
        if (next_idx >= MAX_NODES) return nullptr;
        return &storage[next_idx++];
    }

    void reset() noexcept { next_idx = 0; }
};

extern thread_local NodePool g_pool;

// Builder functions
APEX_API ir::Node* Load(std::string_view field_name) noexcept;
APEX_API ir::Node* Const(int64_t value) noexcept;
APEX_API ir::Node* Add(ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* Sub(ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* GT(ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* LT(ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* GE(ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* LE(ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* EQ(ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* And(ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* Or(ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* Not(ir::Node* a) noexcept;
APEX_API ir::Node* Select(ir::Node* cond, ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* LSL(ir::Node* a, int shift) noexcept;
APEX_API ir::Node* LSR(ir::Node* a, int shift) noexcept;

} // namespace builder

} // namespace apex
