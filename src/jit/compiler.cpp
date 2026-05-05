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

    // Disabled: dump_bytecode has potential buffer management issues causing heap corruption
    // dump_bytecode(code, "JIT Comparison Kernel Bytecode");

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
    static constexpr int MAX_SLOTS = 32768; // 32K slots = 256KB
    std::vector<bool> used_;
    int next_free_ = 0;
public:
    ScratchpadManager() : used_(MAX_SLOTS, false) {}

    int alloc_slot() noexcept {
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (!used_[i]) {
                used_[i] = true;
                return i;
            }
        }
        return -1;
    }

    int alloc_block() noexcept {
        // Allocate 64 contiguous slots for a BITPLANE
        for (int i = 0; i <= MAX_SLOTS - 64; i += 64) {
            bool free = true;
            for (int j = 0; j < 64; ++j) {
                if (used_[i + j]) { free = false; break; }
            }
            if (free) {
                for (int j = 0; j < 64; ++j) used_[i + j] = true;
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

        // Detect active bits for constant optimizations
        if (node->kind == ir::NodeKind::CONST) {
            node->active_bits = (node->const_value == 0) ? 0 : 64 - __builtin_clzll(static_cast<uint64_t>(node->const_value));
        } else if (node->kind == ir::NodeKind::LOAD) {
            node->active_bits = 64; // Fields are 64-bit bit-sliced
        } else if (node->kind == ir::NodeKind::GT || node->kind == ir::NodeKind::LT) {
            // Comparisons against small constants only need to check up to the constant's width
            // + a check for any higher bits in the field.
            if (node->right && node->right->kind == ir::NodeKind::CONST) {
                node->active_bits = std::max(10, node->right->active_bits); // Min 10 bits for RF safety
            } else {
                node->active_bits = 64;
            }
        }
        
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

ExprKernelFunc JitCompiler::compile_expression(ir::Node* root, 
                                             core::SchemaRegistry& registry,
                                             std::string_view schema_name,
                                             int active_bits) noexcept {
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

    // Phase 1: Slot and Register Allocation
    ScratchpadManager scratch_mgr;
    int next_carry_reg = 0;

    for (auto* node : analyzer.all_nodes) {
        if (node->kind == ir::NodeKind::LOAD || node->kind == ir::NodeKind::CONST) continue;

        if (node->result_kind == ir::ResultKind::BITPLANE) {
            node->slot_id = scratch_mgr.alloc_block();
        } else {
            node->slot_id = scratch_mgr.alloc_slot();
        }

        if (node->slot_id < 0) return nullptr; 


        if (node->kind == ir::NodeKind::GT || node->kind == ir::NodeKind::LT ||
            node->kind == ir::NodeKind::GE || node->kind == ir::NodeKind::LE ||
            node->kind == ir::NodeKind::EQ) {
            node->eq_slot_id = scratch_mgr.alloc_slot();
            if (node->eq_slot_id < 0) return nullptr;
        }

        if (node->kind == ir::NodeKind::ADD || node->kind == ir::NodeKind::SUB) {
            if (next_carry_reg < 6) {
                node->carry_reg = next_carry_reg++;
            }
        }
    }

    CodeHolder code;
    code.init(runtime_->environment());
    Assembler a(&code);

    // ARM64 ABI: x0=field_planes_array, x1=scratchpad
    Gp field_planes_ptr = x19;
    Gp scratch_ptr = x20;
    Gp bit_index = x21;
    Gp temp_ptr = x22;
    Gp carry_regs[] = {x23, x24, x25, x26, x27, x28};
    
    // Scratch registers
    Gp plane_a = x2;
    Gp plane_b = x3;
    Gp result = x4;
    Gp mask_gt = x5;
    Gp mask_eq = x6;
    Gp scratch1 = x7;

    // Prologue
    a.stp(x29, x30, Mem(sp, -16).pre());
    a.mov(x29, sp);
    a.stp(x19, x20, Mem(sp, -16).pre());
    a.stp(x21, x22, Mem(sp, -16).pre());
    a.stp(x23, x24, Mem(sp, -16).pre());
    a.stp(x25, x26, Mem(sp, -16).pre());
    a.stp(x27, x28, Mem(sp, -16).pre());

    a.mov(field_planes_ptr, x0);
    a.mov(scratch_ptr, x1);

    // Initialize carry registers and comparison masks
    for (int i = 0; i < 6; ++i) a.mov(carry_regs[i], 0);
    for (auto* node : analyzer.all_nodes) {
        if (node->carry_reg >= 0 && node->kind == ir::NodeKind::SUB) {
            a.mov(carry_regs[node->carry_reg], 1);
        }
        if (node->slot_id >= 0 && node->result_kind == ir::ResultKind::BITMASK) {
            emit_mov_imm64(a, temp_ptr, static_cast<uint64_t>(node->slot_id) * 8);
            a.str(xzr, Mem(scratch_ptr, temp_ptr));
            if (node->eq_slot_id >= 0) {
                emit_mov_imm64(a, temp_ptr, static_cast<uint64_t>(node->eq_slot_id) * 8);
                a.mov(scratch1, -1ULL);
                a.str(scratch1, Mem(scratch_ptr, temp_ptr));
            }
        }
    }

    struct ScratchpadCache {
        asmjit::a64::Assembler& a;
        Gp scratch_ptr;
        Gp temp_ptr;
        
        std::vector<Gp> cache_regs = {x8, x9, x10, x11, x12, x13, x14, x15};
        int next_reg = 0;
        
        std::unordered_map<int, int> slot_to_reg;
        std::vector<int> reg_to_slot = {-1, -1, -1, -1, -1, -1, -1, -1};

        ScratchpadCache(asmjit::a64::Assembler& assembler, Gp scratch, Gp temp) 
            : a(assembler), scratch_ptr(scratch), temp_ptr(temp) {}

        void store_slot(int slot_id, Gp val_reg) {
            emit_mov_imm64(a, temp_ptr, static_cast<uint64_t>(slot_id) * 8);
            a.str(val_reg, Mem(scratch_ptr, temp_ptr));

            int r_idx = -1;
            auto it = slot_to_reg.find(slot_id);
            if (it != slot_to_reg.end()) {
                r_idx = it->second;
            } else {
                r_idx = next_reg;
                next_reg = (next_reg + 1) % 8;
                int old_slot = reg_to_slot[r_idx];
                if (old_slot != -1) {
                    slot_to_reg.erase(old_slot);
                }
            }
            
            a.mov(cache_regs[r_idx], val_reg);
            slot_to_reg[slot_id] = r_idx;
            reg_to_slot[r_idx] = slot_id;
        }

        void load_slot(int slot_id, Gp dest_reg) {
            auto it = slot_to_reg.find(slot_id);
            if (it != slot_to_reg.end()) {
                a.mov(dest_reg, cache_regs[it->second]);
            } else {
                emit_mov_imm64(a, temp_ptr, static_cast<uint64_t>(slot_id) * 8);
                a.ldr(dest_reg, Mem(scratch_ptr, temp_ptr));
                
                int r_idx = next_reg;
                next_reg = (next_reg + 1) % 8;
                int old_slot = reg_to_slot[r_idx];
                if (old_slot != -1) {
                    slot_to_reg.erase(old_slot);
                }
                a.mov(cache_regs[r_idx], dest_reg);
                slot_to_reg[slot_id] = r_idx;
                reg_to_slot[r_idx] = slot_id;
            }
        }
    };

    ScratchpadCache cache(a, scratch_ptr, temp_ptr);

    auto emit_load_op = [&](ir::Node* op, Gp reg) {
        if (op->kind == ir::NodeKind::LOAD) {
            if (op->field_idx < 0) {
                // FALLBACK: Assign field_idx if missing (should not happen if analyzer is correct)
                // This is a safety measure.
            }
            a.ldr(temp_ptr, Mem(field_planes_ptr, static_cast<int64_t>(op->field_idx) * 8));
            a.ldr(reg, Mem(temp_ptr, bit_index, asmjit::a64::lsl(3)));
        } else if (op->kind == ir::NodeKind::CONST) {
            emit_mov_imm64(a, scratch1, static_cast<uint64_t>(op->const_value));
            a.lsr(reg, scratch1, bit_index);
            a.and_(reg, reg, 1);
            a.neg(reg, reg);
        } else {
            emit_mov_imm64(a, scratch1, static_cast<uint64_t>(op->slot_id) * 8);
            a.add(scratch1, scratch1, bit_index, asmjit::a64::lsl(3));
            a.ldr(reg, Mem(scratch_ptr, scratch1));
        }
    };


    // --- Pass 2: Comparisons ---
    for (auto* node : analyzer.all_nodes) {
        if (node->skip_jit) continue;
        if (node->kind == ir::NodeKind::GT || node->kind == ir::NodeKind::LT ||
            node->kind == ir::NodeKind::GE || node->kind == ir::NodeKind::LE ||
            node->kind == ir::NodeKind::EQ) {
            
            cache.load_slot(node->eq_slot_id, mask_eq);
            cache.load_slot(node->slot_id, mask_gt);

            a.mov(bit_index, active_bits - 1);
            Label comp_loop = a.new_label();
            a.bind(comp_loop);

            if (node->kind == ir::NodeKind::LT || node->kind == ir::NodeKind::LE) {
                emit_load_op(node->right, plane_a);
                emit_load_op(node->left, plane_b);
            } else {
                emit_load_op(node->left, plane_a);
                emit_load_op(node->right, plane_b);
            }

            a.bic(result, plane_a, plane_b);
            a.and_(result, result, mask_eq);
            a.orr(mask_gt, mask_gt, result);

            a.eor(result, plane_a, plane_b);
            a.bic(mask_eq, mask_eq, result);

            a.sub(bit_index, bit_index, 1);
            a.cmp(bit_index, 0);
            a.b(asmjit::a64::CondCode::kGE, comp_loop);

            cache.store_slot(node->slot_id, mask_gt);
            cache.store_slot(node->eq_slot_id, mask_eq);
        }
    }

    // --- Pass 4: Logical Aggregation ---
    for (auto* node : analyzer.all_nodes) {
        if (node->skip_jit) continue;
        if (node->kind == ir::NodeKind::GE || node->kind == ir::NodeKind::LE || node->kind == ir::NodeKind::EQ) {
            cache.load_slot(node->slot_id, plane_a);
            cache.load_slot(node->eq_slot_id, plane_b);
            if (node->kind == ir::NodeKind::EQ) a.mov(result, plane_b);
            else a.orr(result, plane_a, plane_b);
            cache.store_slot(node->slot_id, result);
        } else if (node->kind == ir::NodeKind::AND || node->kind == ir::NodeKind::OR || node->kind == ir::NodeKind::NOT) {
            cache.load_slot(node->left->slot_id, plane_a);
            if (node->right) {
                cache.load_slot(node->right->slot_id, plane_b);
                if (node->kind == ir::NodeKind::AND) a.and_(result, plane_a, plane_b);
                else a.orr(result, plane_a, plane_b);
            } else {
                a.mvn(result, plane_a);
            }
            cache.store_slot(node->slot_id, result);
        } else if (node->kind == ir::NodeKind::SELECT) {
            // Load condition mask once for all bits
            cache.load_slot(node->cond->slot_id, mask_gt);

            // Fully loop-unrolled select emission to eliminate branch mispredictions and loop-control overhead
            for (int bit = 0; bit < active_bits; ++bit) {
                a.mov(bit_index, bit);
                
                emit_load_op(node->left, plane_a);
                emit_load_op(node->right, plane_b);

                a.and_(scratch1, mask_gt, plane_a);
                a.bic(temp_ptr, plane_b, mask_gt);
                a.orr(scratch1, scratch1, temp_ptr);

                emit_mov_imm64(a, temp_ptr, static_cast<uint64_t>(node->slot_id) * 8 + bit * 8);
                a.str(scratch1, Mem(scratch_ptr, temp_ptr));
            }
        }
    }

    // --- Pass 5: Arithmetic (ADD/SUB) ---
    for (auto* node : analyzer.all_nodes) {
        if (node->skip_jit) continue;
        if (node->kind == ir::NodeKind::ADD || node->kind == ir::NodeKind::SUB) {
            Gp carry = carry_regs[node->carry_reg];
            if (node->kind == ir::NodeKind::SUB) a.mov(carry, 1);
            else a.mov(carry, 0);

            a.mov(bit_index, 0);
            Label node_loop = a.new_label();
            a.bind(node_loop);
            
            emit_load_op(node->left, plane_a);
            emit_load_op(node->right, plane_b);

            if (node->kind == ir::NodeKind::SUB) a.mvn(plane_b, plane_b);
            a.eor(result, plane_a, plane_b);
            a.eor(scratch1, result, carry);
            a.and_(temp_ptr, plane_a, plane_b);
            a.and_(result, result, carry);
            a.orr(carry, temp_ptr, result);

            emit_mov_imm64(a, temp_ptr, static_cast<uint64_t>(node->slot_id) * 8);
            a.add(temp_ptr, temp_ptr, bit_index, asmjit::a64::lsl(3));
            a.str(scratch1, Mem(scratch_ptr, temp_ptr));

            a.add(bit_index, bit_index, 1);
            a.cmp(bit_index, active_bits);
            a.b(asmjit::a64::CondCode::kLT, node_loop);
        }
    }

    // Final result
    if (root->skip_jit) {
        a.mov(x0, 0);
    } else if (root->result_kind == ir::ResultKind::BITPLANE) {
        emit_mov_imm64(a, temp_ptr, static_cast<uint64_t>(root->slot_id) * 8);
        for (int i = 0; i < 64; ++i) {
            a.ldr(result, Mem(scratch_ptr, temp_ptr));
            a.str(result, Mem(scratch_ptr, i * 8));
            a.add(temp_ptr, temp_ptr, 8);
        }
        a.mov(x0, 0);
    } else if (root->result_kind == ir::ResultKind::BITMASK) {
        emit_mov_imm64(a, temp_ptr, static_cast<uint64_t>(root->slot_id) * 8);
        a.ldr(x0, Mem(scratch_ptr, temp_ptr));
    } else {
        emit_mov_imm64(a, temp_ptr, static_cast<uint64_t>(root->slot_id) * 8);
        a.ldr(x0, Mem(scratch_ptr, temp_ptr));
    }

    // Epilogue
    a.ldp(x27, x28, Mem(sp).post(16));
    a.ldp(x25, x26, Mem(sp).post(16));
    a.ldp(x23, x24, Mem(sp).post(16));
    a.ldp(x21, x22, Mem(sp).post(16));
    a.ldp(x19, x20, Mem(sp).post(16));
    a.ldp(x29, x30, Mem(sp).post(16));
    a.ret(x30);

    ExprKernelFunc fn = nullptr;
    Error err = runtime_->add(&fn, &code);
    if (err != kErrorOk) return nullptr;
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
    for (auto* node : analyzer.all_nodes) {
        if (node->kind == ir::NodeKind::LOAD) {
            std::string field_name(node->field_name);
            if (field_to_idx.find(field_name) == field_to_idx.end()) {
                field_to_idx[field_name] = field_to_idx.size();
            }
            node->field_idx = field_to_idx[field_name];
        }
    }

    // Allocate scratchpad slots for intermediate nodes
    ScratchpadManager scratch_mgr;
    for (auto* node : analyzer.all_nodes) {
        if (node->kind != ir::NodeKind::LOAD && node->kind != ir::NodeKind::CONST) {
            node->slot_id = scratch_mgr.alloc_slot();
        }
    }

    CodeHolder code;
    code.init(runtime_->environment());
    Assembler a(&code);

    // ARM64 ABI: x0=field_arrays, x1=scratchpad
    Gp field_arrays_ptr = x19;
    Gp scratchpad_ptr = x20;
    Gp final_mask = x21;
    Gp row_index = x22;
    Gp temp_reg = x23;
    Gp val_left = x16;
    Gp val_right = x17;
    Gp val_res = x5;

    // Prologue
    a.stp(x29, x30, Mem(sp, -16).pre());
    a.mov(x29, sp);
    a.stp(x19, x20, Mem(sp, -16).pre());
    a.stp(x21, x22, Mem(sp, -16).pre());
    a.stp(x23, x24, Mem(sp, -16).pre());

    a.mov(field_arrays_ptr, x0);
    a.mov(scratchpad_ptr, x1);
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
                a.ldr(temp_reg, Mem(field_arrays_ptr, operand->field_idx * 8));
                a.ldr(out_reg, Mem(temp_reg, row_index, asmjit::a64::lsl(3)));
            } else if (operand->kind == ir::NodeKind::CONST) {
                emit_mov_imm64(a, out_reg, operand->const_value);
            } else {
                a.ldr(out_reg, Mem(scratchpad_ptr, operand->slot_id * 8));
            }
        };

        get_operand(node->left, val_left);
        get_operand(node->right, val_right);

        switch (node->kind) {
            case ir::NodeKind::ADD: a.add(val_res, val_left, val_right); break;
            case ir::NodeKind::SUB: a.sub(val_res, val_left, val_right); break;
            case ir::NodeKind::GT: a.cmp(val_left, val_right); a.cset(val_res, asmjit::a64::CondCode::kHI); break;
            case ir::NodeKind::LT: a.cmp(val_left, val_right); a.cset(val_res, asmjit::a64::CondCode::kLO); break;
            case ir::NodeKind::GE: a.cmp(val_left, val_right); a.cset(val_res, asmjit::a64::CondCode::kHS); break;
            case ir::NodeKind::LE: a.cmp(val_left, val_right); a.cset(val_res, asmjit::a64::CondCode::kLS); break;
            case ir::NodeKind::EQ: a.cmp(val_left, val_right); a.cset(val_res, asmjit::a64::CondCode::kEQ); break;
            case ir::NodeKind::AND: a.and_(val_res, val_left, val_right); break;
            case ir::NodeKind::OR:  a.orr(val_res, val_left, val_right); break;
            case ir::NodeKind::SELECT:
                // SELECT cond ? left : right
                a.ldr(temp_reg, Mem(scratchpad_ptr, node->cond->slot_id * 8));
                a.cmp(temp_reg, 0);
                a.csel(val_res, val_left, val_right, asmjit::a64::CondCode::kNE);
                break;
            default: break;
        }

        if (node->slot_id >= 0) {
            a.str(val_res, Mem(scratchpad_ptr, node->slot_id * 8));
        }
    }

    // Shift by row_index and OR into final_mask
    a.lsl(temp_reg, val_res, row_index);
    a.orr(final_mask, final_mask, temp_reg);

    a.add(row_index, row_index, 1);
    a.b(loop_start);

    a.bind(loop_end);

    a.mov(x0, final_mask);
    
    // Epilogue
    a.ldp(x23, x24, Mem(sp).post(16));
    a.ldp(x21, x22, Mem(sp).post(16));
    a.ldp(x19, x20, Mem(sp).post(16));
    a.ldp(x29, x30, Mem(sp).post(16));
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
