#pragma once

#include <string>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <string_view>
#include <cstdint>
#include "apex/common.hpp"
#include <new>
#include <cstdlib>

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
    MUL,      // Arithmetic multiplication (Shift-and-Add)
    POPCNT,   // Popcount of mask (Voting)
    LSL,      // Logical shift left (index remap only)
    LSR,      // Logical shift right (index remap only)
    SUM,      // Multi-operand summation (Wallace Tree reduction)
    INFERENCE_COUNT, // Convert BITMASK to BITPLANE (mask -> plane bit 0)
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
    int64_t weight = 0;           // For nodes used in SUM: leaf weight
    int shift_amount = 0;         // For LSL/LSR: number of bits to shift

    // Tree structure
    Node* left = nullptr;         // Left operand
    Node* right = nullptr;        // Right operand
    Node* cond = nullptr;         // For SELECT: condition
    std::vector<Node*> operands;  // For SUM: variadic list of trees

    // Compile-time metadata (filled by JitCompiler during analysis)
    int slot_id = -1;             // Scratchpad slot ID (BITPLANE/BITMASK result)
    int eq_slot_id = -1;          // Scratchpad slot ID for EQ mask (Comparison nodes)
    int field_idx = -1;           // Field index in field_planes array (LOAD nodes)
    int carry_reg = -1;           // Which callee-saved register holds carry (x23-x28)
    bool skip_jit = false;        // If true, JIT compiler skips this node
    int active_bits = 64;         // Max bits to evaluate (optimization)
};

} // namespace ir

namespace builder {

// Global node pool for tree construction (setup-time only, no heap during execute)
struct NodePool {
    static constexpr int MAX_NODES = 2097152; // 2M nodes
    ir::Node* storage;
    int next_idx = 0;

    NodePool() noexcept : next_idx(0) {
        storage = (ir::Node*)std::malloc(MAX_NODES * sizeof(ir::Node));
        if (storage) {
        } else {
            fprintf(stderr, "[AARCHGATE DEBUG] ERROR: NodePool Malloc FAILED!\n"); fflush(stderr);
        }
    }

    ~NodePool() noexcept {
        reset();
        std::free(storage);
    }

    ir::Node* alloc() noexcept {
        if (!storage || next_idx >= MAX_NODES) return nullptr;
        ir::Node* n = &storage[next_idx++];
        return new (n) ir::Node(); // Placement new to initialize std::vector
    }

    void reset() noexcept {
        if (!storage) return;
        for (int i = 0; i < next_idx; ++i) {
            storage[i].~Node(); // Call destructor to free std::vector memory
        }
        next_idx = 0; 
    }
};

extern NodePool* g_pool;

// Builder functions
APEX_API ir::Node* Load(const char* field_name) noexcept;
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
APEX_API ir::Node* Mul(ir::Node* a, ir::Node* b) noexcept;
APEX_API ir::Node* Popcnt(ir::Node* a) noexcept;
APEX_API ir::Node* LSL(ir::Node* a, int shift) noexcept;
APEX_API ir::Node* LSR(ir::Node* a, int shift) noexcept;
APEX_API ir::Node* Sum(const std::vector<ir::Node*>& operands) noexcept;
APEX_API ir::Node* InferenceCount(ir::Node* cond) noexcept;

} // namespace builder

} // namespace apex
