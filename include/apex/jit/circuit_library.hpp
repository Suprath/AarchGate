#pragma once

#include <asmjit/a64.h>

namespace apex {
namespace jit {

class CircuitLibrary {
public:
    static void emit_gt_64(
        asmjit::a64::Assembler& a,
        const asmjit::a64::Gp& a_bit,
        const asmjit::a64::Gp& b_bit,
        asmjit::a64::Gp& gt_mask,
        asmjit::a64::Gp& eq_mask) noexcept;

    static void emit_add_64(
        asmjit::a64::Assembler& a,
        const asmjit::a64::Gp& a_plane,
        const asmjit::a64::Gp& b_plane,
        asmjit::a64::Gp& carry,
        asmjit::a64::Gp& sum,
        const asmjit::a64::Gp& tmp1,
        const asmjit::a64::Gp& tmp2) noexcept;

    // Carry-Save Adder (Wallace Tree building block)
    // Compresses 3 inputs into 2 (sum and carry)
    static void emit_csa(
        asmjit::a64::Assembler& a,
        const asmjit::a64::Gp& a_in,
        const asmjit::a64::Gp& b_in,
        const asmjit::a64::Gp& c_in,
        asmjit::a64::Gp& sum_out,
        asmjit::a64::Gp& carry_out,
        const asmjit::a64::Gp& tmp) noexcept;

    // Arithmetic: Bit-Sliced Multiplier (Constant Optimization)
    static void emit_mul_64(
        asmjit::a64::Assembler& a,
        const asmjit::a64::Gp& a_plane,
        int64_t constant,
        int bit_index,
        asmjit::a64::Gp* carries, // Array of carries (one per set bit in constant)
        asmjit::a64::Gp& sum,
        const asmjit::a64::Gp& tmp1,
        const asmjit::a64::Gp& tmp2) noexcept;

    // MUX/SELECT: out = (cond & a) | (~cond & b)
    static void emit_mux(
        asmjit::a64::Assembler& a,
        const asmjit::a64::Gp& cond,
        const asmjit::a64::Gp& a_mask,
        const asmjit::a64::Gp& b_mask,
        asmjit::a64::Gp& out,
        const asmjit::a64::Gp& tmp) noexcept;

    // POPCNT: Sum across 64-bit mask using NEON
    static void emit_popcnt_mask(
        asmjit::a64::Assembler& a,
        const asmjit::a64::Gp& mask,
        asmjit::a64::Gp& out) noexcept;
};

} // namespace jit
} // namespace apex
