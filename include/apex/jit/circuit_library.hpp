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

    // MUX/SELECT: out = (cond & a) | (~cond & b)
    static void emit_mux(
        asmjit::a64::Assembler& a,
        const asmjit::a64::Gp& cond,
        const asmjit::a64::Gp& a_mask,
        const asmjit::a64::Gp& b_mask,
        asmjit::a64::Gp& out,
        const asmjit::a64::Gp& tmp) noexcept;
};

} // namespace jit
} // namespace apex
