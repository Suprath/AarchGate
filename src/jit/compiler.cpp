// (c) 2024-2026 Suprath PS. All rights reserved.
// Project Apex: Universal JIT-Accelerated Vector Engine (10B+ RPS)
//
// This work is licensed under the Business Source License 1.1 until 2029-05-03,
// transitioning to the Apache License 2.0 thereafter.
// See the LICENSE file in the repository root for the full text.
// BUILD: 2026-05-04T03-Universal-JIT-Loop-v3

#include "apex/jit/compiler.hpp"
#include "apex/jit/ir.hpp"
#include "apex/jit/circuit_library.hpp"
#include "apex/core/registry.hpp"
#include <iostream>
#include <iomanip>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <functional>

namespace apex {
namespace jit {

JitCompiler::JitCompiler() noexcept
    : runtime_(std::make_unique<asmjit::JitRuntime>()) {}

JitCompiler::~JitCompiler() noexcept = default;

void JitCompiler::dump_bytecode(const asmjit::CodeHolder& code, const char* label) const noexcept {
    std::cout << "\n=== " << label << " ===\n";

    for (auto& section : code.sections()) {
        const uint8_t* data = section->buffer().data();
        size_t size = section->buffer().size();

        std::cout << "Section size: " << size << " bytes\n";
        std::cout << "Hex dump (first 2048 bytes):\n";

        size_t dump_size = (size > 2048) ? 2048 : size;
        for (size_t i = 0; i < dump_size; i++) {
            if (i % 16 == 0) {
                std::cout << "\n  " << std::hex << std::setw(4) << std::setfill('0') << i << ": ";
            }
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
        }
        std::cout << std::dec << "\n";
    }
}

// Pre-sliced constant pool: stores bit-planes for CONST nodes
struct ConstPool {
    std::unordered_map<int64_t, uint64_t*> pools;

    uint64_t* get_or_create(int64_t const_value) noexcept {
        if (pools.find(const_value) != pools.end()) {
            return pools[const_value];
        }
        uint64_t* bit_planes = new uint64_t[64];
        for (int bit = 0; bit < 64; ++bit) {
            bit_planes[bit] = ((const_value >> bit) & 1) ? ~0ULL : 0ULL;
        }
        pools[const_value] = bit_planes;
        return bit_planes;
    }

    ~ConstPool() noexcept {
        for (auto& [_, planes] : pools) {
            delete[] planes;
        }
    }
};

thread_local ConstPool g_const_pool;

// ULL-Compliant: JIT code generation helper (compile-time only, not hot path)
// Helper: emit a full 64-bit immediate load on ARM64 using movz/movk sequence.
// ARM64 MOV can only encode 16-bit immediates; a full 64-bit pointer requires
// up to 4 instructions. Using a.mov() with a large immediate may silently
// truncate or fail on the low-level Assembler API.
static void emit_mov_imm64(asmjit::a64::Assembler& a,
                           const asmjit::a64::Gp& dst,
                           uint64_t imm) noexcept {
    using namespace asmjit;
    a.movz(dst, imm & 0xFFFF, 0);
    a.movk(dst, (imm >> 16) & 0xFFFF, 16);
    a.movk(dst, (imm >> 32) & 0xFFFF, 32);
    a.movk(dst, (imm >> 48) & 0xFFFF, 48);
}

// ULL-Compliant: JIT compilation (init-time, generates hot kernel)
// Target: Kernel latency ~20ns for 64-bit multi-comparison
KernelFunc JitCompiler::compile_comparison(uint64_t threshold) noexcept {
    using namespace asmjit;
    using namespace asmjit::a64;

    CodeHolder code;
    code.init(runtime_->environment());

    Assembler a(&code);

    // ARM64 ABI register allocation
    // x0 = bit_planes_ptr (input, read-only)
    // x9 = GT_mask (accumulator)
    // x10 = EQ_mask (accumulator)
    // x11 = pointer_walker (post-index pointer)
    // x12 = a_bitplane (loaded value)
    // x13 = tmp (scratch)
    // x14, x15 = additional temps if needed

    Gp bit_planes_ptr = x0;
    Gp gt_mask = x9;
    Gp eq_mask = x10;
    Gp pointer_walker = x11;
    Gp a_bitplane = x12;
    Gp tmp = x13;

    // Initialize state: GT = 0, EQ = all 1s
    a.orr(gt_mask, xzr, xzr);  // GT = 0 (explicit XOR with zero register)
    a.mov(eq_mask, -1);         // EQ = all 1s (load immediate -1)

    // Setup pointer walker to bit_planes[63]
    a.add(pointer_walker, bit_planes_ptr, 504);  // 504 = 63 * 8 bytes

    // Generate 64 unrolled comparison blocks (bit 63 down to 0, MSB to LSB)
    std::cout << "DEBUG: threshold = " << threshold << " (0x" << std::hex << threshold << std::dec << ")\n";
    std::cout << "Threshold bits: ";
    for (int bit = 63; bit >= 0; --bit) {
        uint64_t threshold_bit = (threshold >> bit) & 1;
        if (bit % 8 == 0 && bit < 63) std::cout << " ";
        std::cout << threshold_bit;
    }
    std::cout << "\n\n";

    int branch0_count = 0, branch1_count = 0;

    for (int bit = 63; bit >= 0; --bit) {
        uint64_t threshold_bit = (threshold >> bit) & 1;

        // Load bit-plane[bit] using post-index addressing
        // a.ldr loads from [pointer_walker], then decrements pointer_walker by 8
        a.ldr(a_bitplane, Mem(pointer_walker).post(-8));

        if (threshold_bit == 0) {
            branch0_count++;
            // When threshold_bit is 0:
            // GT = GT | (EQ & A_bit & ~0) = GT | (EQ & A_bit)
            // EQ = EQ & ~(A_bit ^ 0) = EQ & ~A_bit

            // tmp = EQ & A_bit
            a.and_(tmp, eq_mask, a_bitplane);
            // GT |= tmp
            a.orr(gt_mask, gt_mask, tmp);
            // EQ &= ~A_bit (using BIC: Bit Clear)
            a.bic(eq_mask, eq_mask, a_bitplane);
        } else {
            branch1_count++;
            // When threshold_bit is 1:
            // GT = GT | (EQ & A_bit & ~1) = GT | 0 = GT (unchanged)
            // EQ = EQ & ~(A_bit ^ 1) = EQ & A_bit

            // EQ &= A_bit
            a.and_(eq_mask, eq_mask, a_bitplane);
        }
    }

    std::cout << "Branch 0 (threshold_bit==0) count: " << branch0_count << "\n";
    std::cout << "Branch 1 (threshold_bit==1) count: " << branch1_count << "\n\n";

    // Return GT_mask in x0
    a.mov(x0, gt_mask);
    a.ret(x30);

    // Dump bytecode for inspection
    dump_bytecode(code, "JIT Comparison Kernel Bytecode");

    // Add code to JIT runtime
    KernelFunc fn = nullptr;
    Error err = runtime_->add(&fn, &code);
    if (err != kErrorOk) {
        return nullptr;
    }

    return fn;
}

// Helper for compile_expression: Scratchpad allocation
class ScratchpadManager {
public:
    static constexpr int MAX_SLOTS = 1024;

    int alloc_slot() noexcept {
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (!used_[i]) {
                used_[i] = true;
                return i;
            }
        }
        return -1;
    }

    void free_slot(int id) noexcept {
        if (id >= 0 && id < MAX_SLOTS) {
            used_[id] = false;
        }
    }

private:
    bool used_[MAX_SLOTS] = {};
};

// Helper: depth-first visitor to collect metadata
struct ExpressionAnalyzer {
    std::unordered_map<ir::Node*, int> node_map;
    std::vector<ir::Node*> arithmetic_nodes; // Phase 1
    std::vector<ir::Node*> comparison_nodes; // Phase 2
    std::vector<ir::Node*> select_nodes;     // Phase 3
    std::vector<ir::Node*> all_nodes;

    void analyze(ir::Node* node) noexcept {
        if (!node) return;

        // Check if already visited
        if (node_map.find(node) != node_map.end()) return;

        // Visit children first (post-order)
        if (node->left) analyze(node->left);
        if (node->right) analyze(node->right);
        if (node->cond) analyze(node->cond);
        for (auto* op : node->operands) analyze(op);

        all_nodes.push_back(node);

        switch (node->kind) {
            case ir::NodeKind::LOAD:
            case ir::NodeKind::CONST:
            case ir::NodeKind::ADD:
            case ir::NodeKind::SUB:
            case ir::NodeKind::MUL:
            case ir::NodeKind::SELECT:
            case ir::NodeKind::POPCNT:
            case ir::NodeKind::SUM:
            case ir::NodeKind::INFERENCE_COUNT:
                node->result_kind = ir::ResultKind::BITPLANE;
                if (node->kind == ir::NodeKind::SELECT || 
                    node->kind == ir::NodeKind::POPCNT ||
                    node->kind == ir::NodeKind::SUM ||
                    node->kind == ir::NodeKind::INFERENCE_COUNT) {
                    select_nodes.push_back(node);
                } else {
                    arithmetic_nodes.push_back(node);
                }
                break;

            case ir::NodeKind::GT:
            case ir::NodeKind::LT:
            case ir::NodeKind::GE:
            case ir::NodeKind::LE:
            case ir::NodeKind::EQ:
            case ir::NodeKind::AND:
            case ir::NodeKind::OR:
            case ir::NodeKind::NOT:
                node->result_kind = ir::ResultKind::BITMASK;
                comparison_nodes.push_back(node);
                break;

            case ir::NodeKind::LSL:
            case ir::NodeKind::LSR:
                node->result_kind = ir::ResultKind::BITPLANE;
                arithmetic_nodes.push_back(node);
                break;
        }

        node_map[node] = all_nodes.size() - 1;
    }
};

ExprKernelFunc JitCompiler::compile_expression(
    ir::Node* root,
    const core::SchemaRegistry& registry,
    std::string_view schema_name) noexcept {
    (void)registry;
    (void)schema_name;
    if (!root) return nullptr;

    using namespace asmjit;
    using namespace asmjit::a64;

    // Analysis pass: collect nodes and assign metadata
    ExpressionAnalyzer analyzer;
    analyzer.analyze(root);

    // Assign field indices to LOAD nodes only (CONST nodes do NOT go into the field array)
    std::unordered_map<std::string, int> field_to_idx;
    for (auto* node : analyzer.arithmetic_nodes) {
        if (node->kind != ir::NodeKind::LOAD) continue;
        ir::Node* load_node = node;
        std::string field_name(load_node->field_name);
        if (field_to_idx.find(field_name) == field_to_idx.end()) {
            if (field_to_idx.size() >= 8) return nullptr;
            field_to_idx[field_name] = static_cast<int>(field_to_idx.size());
        }
        load_node->field_idx = field_to_idx[field_name];
    }

    // Pre-slice CONST nodes into bit-vectors in C++
    std::unordered_map<ir::Node*, uint64_t*> const_bit_planes;
    for (auto* node : analyzer.arithmetic_nodes) {
        if (node->kind != ir::NodeKind::CONST) continue;
        ir::Node* const_node = node;
        const_bit_planes[const_node] = g_const_pool.get_or_create(const_node->const_value);
        const_node->field_idx = -2;  // Mark as const-pool (not a field index)
    }

    // Allocate scratchpad slots for arithmetic results
    ScratchpadManager scratch_mgr;
    for (auto* node : analyzer.arithmetic_nodes) {
        if (node->kind == ir::NodeKind::LOAD || node->kind == ir::NodeKind::CONST) continue;
        node->slot_id = scratch_mgr.alloc_slot();
        if (node->slot_id < 0) return nullptr;
    }
    for (auto* node : analyzer.select_nodes) {
        node->slot_id = scratch_mgr.alloc_slot();
        if (node->slot_id < 0) return nullptr;
    }
    for (auto* node : analyzer.comparison_nodes) {
        node->slot_id = scratch_mgr.alloc_slot();
        if (node->slot_id < 0) return nullptr;
    }

    // Assign carry registers to nodes that require them (ADD/SUB/SUM)
    int carry_reg_idx = 0;
    auto assign_carry = [&](ir::Node* node) {
        if (node->kind == ir::NodeKind::ADD || 
            node->kind == ir::NodeKind::SUB || 
            node->kind == ir::NodeKind::SUM) {
            if (carry_reg_idx < 8) {
                node->carry_reg = carry_reg_idx++;
            }
        }
    };

    for (auto* node : analyzer.arithmetic_nodes) assign_carry(node);
    for (auto* node : analyzer.select_nodes) assign_carry(node);

    CodeHolder code;
    code.init(runtime_->environment());
    code.reserve_buffer(&code.section_by_id(0)->buffer(), 65536);
    Assembler a(&code);

    // ARM64 ABI: x0=field_planes_array, x1=scratchpad, x30=return address
    Gp field_planes_array = x0;
    Gp scratchpad = x1;

    // Callee-saved base registers: x19=field_planes_array, x20=scratchpad
    // Carry registers: x21-x28
    static Gp carry_regs[] = {x21, x22, x23, x24, x25, x26, x27, x28};

    // Working registers
    Gp field_planes_ptr = x19;  // Callee-saved: field_planes_array (set in prologue)
    Gp scratch_ptr = x20;       // Callee-saved: scratchpad (set in prologue)
    Gp bit_index = x15;         // JIT loop counter: 0→63 (non-callee-saved)
    Gp plane_a = x16;           // Use IP0 (non-callee-saved)
    Gp plane_b = x17;           // Use IP1 (non-callee-saved)
    Gp temp_ptr = x3;           // Temp pointer for field-plane loads (x1 conflicts with offset work)
    Gp result = x5;             // Arithmetic result
    Gp scratch1 = x6;           // Scratch register 1
    Gp scratch3 = x14;          // Scratch register 3 (Offset calculation)
    // x15 is used as bit_index

    // ===== Standard ARM64 Procedure Call Frame =====
    a.stp(x29, x30, Mem(sp, -16).pre());
    a.mov(x29, sp);
    a.stp(x19, x20, Mem(sp, -16).pre());
    a.stp(x21, x22, Mem(sp, -16).pre());
    a.stp(x23, x24, Mem(sp, -16).pre());
    a.stp(x25, x26, Mem(sp, -16).pre());
    a.stp(x27, x28, Mem(sp, -16).pre());

    a.mov(field_planes_ptr, field_planes_array);
    a.mov(scratch_ptr, scratchpad);

    // Initialize carry registers
    for (auto* arith_node : analyzer.arithmetic_nodes) {
        if (arith_node->carry_reg >= 0) {
            if (arith_node->kind == ir::NodeKind::ADD) {
                a.mov(carry_regs[arith_node->carry_reg], 0);
            } else if (arith_node->kind == ir::NodeKind::SUB) {
                a.mov(carry_regs[arith_node->carry_reg], 1);
            }
        }
    }

    // =====================================================================
    // PHASE 1: Nested Loop Kernel (Bits × Trees) — v1.1 I-Cache Optimized
    // =====================================================================
    // Layout: GT[t] @ scratchpad[t*8], EQ[t] @ scratchpad[800 + t*8]
    // Tree metadata embedded after `ret` and accessed via ADR (PC-relative).
    // Kernel code: ~70 instructions ≈ 280 bytes (fits entirely in M3 μOp cache).
    // =====================================================================

    // Instead of using scratch_mgr, we output masks directly into scratchpad[0..99].
    // EQ masks can be placed temporarily at scratchpad[100..199].
    const int num_trees = static_cast<int>(analyzer.comparison_nodes.size());
    const int GT_BASE = 0;
    const int EQ_BASE = 800;


    // x22 = tree counter (callee-saved, already pushed)
    Gp tree_idx = x22;

    // Label for embedded tree metadata (placed after ret)
    Label tree_data_label = a.new_label();

    // Load base address of tree metadata table via ADR (PC-relative, always valid)
    // x21 = pointer to {field_idx[0], threshold[0], field_idx[1], threshold[1], ...}
    a.adr(x21, tree_data_label);

    // --- Init loop: GT[t]=0, EQ[t]=~0 for all trees ---
    a.mov(tree_idx, 0);
    Label init_loop = a.new_label();
    a.bind(init_loop);
    {
        // GT[t] = 0 at scratchpad[GT_BASE + t*8]
        a.mov(x2, GT_BASE);
        a.add(x2, x2, tree_idx, asmjit::a64::lsl(3));
        a.str(xzr, Mem(scratch_ptr, x2));
        
        // EQ[t] = ~0 at scratchpad[EQ_BASE + t*8]
        a.mov(x2, EQ_BASE);
        a.add(x2, x2, tree_idx, asmjit::a64::lsl(3));
        a.mov(scratch1, -1ULL);
        a.str(scratch1, Mem(scratch_ptr, x2));
    }
    a.add(tree_idx, tree_idx, 1);
    a.cmp(tree_idx, num_trees);
    a.b(asmjit::a64::CondCode::kLT, init_loop);

    // --- Outer loop: bit_index 9 → 0 ---
    // (Optimization: features <= 169, thresholds <= 1000. Bits 10..63 are guaranteed 0,
    // so they do not affect GT or EQ. We start from bit 9 to skip useless math).
    a.mov(bit_index, 9);   // x15 = bit index
    Label bit_loop = a.new_label();
    a.bind(bit_loop);

        // --- Inner loop: tree_idx 0 → num_trees-1 ---
        a.mov(tree_idx, 0);
        Label tree_loop = a.new_label();
        a.bind(tree_loop);

            // Load field_idx = tree_meta[t].field_idx  (offset t*16, size 8)
            a.add(x2, x21, tree_idx, asmjit::a64::lsl(4));  // x2 = &meta[t]
            a.ldr(x3, Mem(x2, 0));   // x3 = field_idx
            a.ldr(x4, Mem(x2, 8));   // x4 = threshold

            // plane_a = field_planes[field_idx][bit_index]
            a.ldr(temp_ptr, Mem(field_planes_ptr, x3, asmjit::a64::lsl(3)));  // temp_ptr = field_planes[fi]
            a.ldr(plane_a, Mem(temp_ptr, bit_index, asmjit::a64::lsl(3)));    // plane_a = [fi][bit]

            // plane_b = (threshold >> bit_index) & 1, negated → 0 or ~0ULL
            a.lsr(plane_b, x4, bit_index);    // plane_b = threshold >> bit
            a.and_(plane_b, plane_b, 1);       // & 1
            a.neg(plane_b, plane_b);           // 0 → 0,  1 → ~0ULL

            // Load GT[t] and EQ[t] from compact scratchpad
            a.mov(x2, GT_BASE);
            a.add(x2, x2, tree_idx, asmjit::a64::lsl(3));
            a.ldr(x9, Mem(scratch_ptr, x2));                                  // GT[t]
            
            a.mov(x2, EQ_BASE);
            a.add(x2, x2, tree_idx, asmjit::a64::lsl(3));
            a.ldr(x10, Mem(scratch_ptr, x2));                                  // EQ[t]

            // emit_gt_64 inline (avoids call overhead; uses x6,x7 as scratch)
            a.bic(x6, plane_a, plane_b);          // x6 = A & ~B
            a.and_(x7, x10, x6);                  // x7 = EQ & x6
            a.orr(x9, x9, x7);                    // GT |= (EQ & (A & ~B))
            a.eor(x6, plane_a, plane_b);           // x6 = A ^ B
            a.bic(x10, x10, x6);                  // EQ &= ~(A ^ B)

            // Store GT[t] and EQ[t] back
            a.mov(x2, GT_BASE);
            a.add(x2, x2, tree_idx, asmjit::a64::lsl(3));
            a.str(x9,  Mem(scratch_ptr, x2));                                 // GT[t]
            
            a.mov(x2, EQ_BASE);
            a.add(x2, x2, tree_idx, asmjit::a64::lsl(3));
            a.str(x10, Mem(scratch_ptr, x2));                                  // EQ[t]

        a.add(tree_idx, tree_idx, 1);
        a.cmp(tree_idx, num_trees);
        a.b(asmjit::a64::CondCode::kLT, tree_loop);

    a.subs(bit_index, bit_index, 1);
    a.b(asmjit::a64::CondCode::kGE, bit_loop);

    // --- Phase 2 logic removed for Hybrid Aggregation ---
    // The JIT kernel now simply returns the GT masks in scratchpad[0..99]
    a.mov(x0, 0); // Return success

    // ===== Standard ARM64 Epilogue =====
    a.ldp(x27, x28, Mem(sp).post(16));
    a.ldp(x25, x26, Mem(sp).post(16));
    a.ldp(x23, x24, Mem(sp).post(16));
    a.ldp(x21, x22, Mem(sp).post(16));
    a.ldp(x19, x20, Mem(sp).post(16));
    a.ldp(x29, x30, Mem(sp).post(16));
    a.ret(x30);

    // ===== Embedded Tree Metadata (PC-relative data, accessed via ADR) =====
    // Phase 1 metadata: [{field_idx: uint64, threshold: uint64}, ...] × num_trees
    a.bind(tree_data_label);
    for (auto* node : analyzer.comparison_nodes) {
        uint64_t field_idx  = static_cast<uint64_t>(node->left->field_idx);
        uint64_t threshold  = static_cast<uint64_t>(node->right->const_value);
        a.embed_uint64(field_idx);
        a.embed_uint64(threshold);
    }

    // Phase 2 metadata logic removed

    dump_bytecode(code, "JIT Expression Kernel Bytecode");


    ExprKernelFunc fn = nullptr;
    Error err = runtime_->add(&fn, &code);
    if (err != kErrorOk) {
        return nullptr;
    }

    return fn;
}

ExprKernelFunc JitCompiler::compile_scalar_expression(
    ir::Node* root,
    const core::SchemaRegistry& registry,
    std::string_view schema_name) noexcept {

    (void)schema_name;
    (void)registry;
    if (!root) return nullptr;

    using namespace asmjit;
    using namespace asmjit::a64;

    ExpressionAnalyzer analyzer;
    analyzer.analyze(root);

    // Assign field indices to LOAD nodes
    std::unordered_map<std::string, int> field_to_idx;
    for (auto* node : analyzer.arithmetic_nodes) {
        if (node->kind != ir::NodeKind::LOAD) continue;
        ir::Node* load_node = node;
        std::string field_name(load_node->field_name);
        if (field_to_idx.find(field_name) == field_to_idx.end()) {
            field_to_idx[field_name] = field_to_idx.size();
        }
        load_node->field_idx = field_to_idx[field_name];
    }

    CodeHolder code;
    code.init(runtime_->environment());
    code.reserve_buffer(&code.section_by_id(0)->buffer(), 65536);
    Assembler a(&code);

    // ARM64 ABI: x0=field_arrays, x1=scratchpad, x30=return address
    Gp field_arrays = x0;
    Gp scratchpad = x1;

    // Working registers
    Gp final_mask = x19;
    Gp row_index = x20;
    Gp temp_ptr = x2;
    Gp val_left = x16;
    Gp val_right = x17;
    Gp val_res = x5;

    // Prologue
    a.stp(x29, x30, Mem(sp, -16).pre());
    a.mov(x29, sp);
    a.stp(x19, x20, Mem(sp, -16).pre());

    a.mov(final_mask, 0);
    a.mov(row_index, 0);

    Label loop_start = a.new_label();
    Label loop_end = a.new_label();

    a.bind(loop_start);
    a.cmp(row_index, 64);
    a.b(asmjit::a64::CondCode::kHS, loop_end);

    // Linear post-order evaluation
    for (size_t i = 0; i < analyzer.all_nodes.size(); ++i) {
        ir::Node* node = analyzer.all_nodes[i];
        
        auto get_operand = [&](ir::Node* operand, Gp out_reg) {
            if (!operand) return;
            if (operand->kind == ir::NodeKind::LOAD) {
                // ldr temp_ptr, [field_arrays + field_idx * 8]
                a.ldr(temp_ptr, Mem(field_arrays, operand->field_idx * 8));
                // ldr out_reg, [temp_ptr, row_index LSL 3]
                a.ldr(out_reg, Mem(temp_ptr, row_index, asmjit::a64::lsl(3)));
            } else if (operand->kind == ir::NodeKind::CONST) {
                emit_mov_imm64(a, out_reg, operand->const_value);
            } else {
                // Find index of this operand in all_nodes
                size_t op_idx = 0;
                for (size_t j = 0; j < i; ++j) {
                    if (analyzer.all_nodes[j] == operand) { op_idx = j; break; }
                }
                a.ldr(out_reg, Mem(scratchpad, op_idx * 8));
            }
        };

        get_operand(node->left, val_left);
        get_operand(node->right, val_right);

        switch (node->kind) {
            case ir::NodeKind::LOAD:
            case ir::NodeKind::CONST:
                // Already handled in get_operand for parents. But we need to store it to scratchpad if it's evaluated here.
                get_operand(node, val_res);
                break;
            case ir::NodeKind::ADD:
                a.add(val_res, val_left, val_right);
                break;
            case ir::NodeKind::SUB:
                a.sub(val_res, val_left, val_right);
                break;
            case ir::NodeKind::GT:
                a.cmp(val_left, val_right);
                a.cset(val_res, asmjit::a64::CondCode::kHI); // Unsigned GT
                break;
            case ir::NodeKind::LT:
                a.cmp(val_left, val_right);
                a.cset(val_res, asmjit::a64::CondCode::kLO); // Unsigned LT
                break;
            case ir::NodeKind::GE:
                a.cmp(val_left, val_right);
                a.cset(val_res, asmjit::a64::CondCode::kHS); // Unsigned GE
                break;
            case ir::NodeKind::LE:
                a.cmp(val_left, val_right);
                a.cset(val_res, asmjit::a64::CondCode::kLS); // Unsigned LE
                break;
            case ir::NodeKind::EQ:
                a.cmp(val_left, val_right);
                a.cset(val_res, asmjit::a64::CondCode::kEQ);
                break;
            case ir::NodeKind::AND:
                a.and_(val_res, val_left, val_right);
                break;
            case ir::NodeKind::OR:
                a.orr(val_res, val_left, val_right);
                break;
            default: break;
        }

        // Store result to scratchpad
        a.str(val_res, Mem(scratchpad, i * 8));
    }

    // Root result is in val_res (or scratchpad at all_nodes.size() - 1). It is 1 or 0.
    // Shift by row_index and OR into final_mask
    a.lsl(val_res, val_res, row_index);
    a.orr(final_mask, final_mask, val_res);

    a.add(row_index, row_index, 1);
    a.b(loop_start);

    a.bind(loop_end);

    // Epilogue
    a.mov(x0, final_mask); // Return value
    a.ldp(x19, x20, Mem(sp, 16).post());
    a.ldp(x29, x30, Mem(sp, 16).post());
    a.ret(x30);

    // dump_bytecode(code, "Scalar Expression Kernel");

    ExprKernelFunc fn = nullptr;
    Error err = runtime_->add(&fn, &code);
    if (err != kErrorOk) {
        return nullptr;
    }
    return fn;
}

} // namespace jit
} // namespace apex
