// (c) 2024-2026 Suprath PS. All rights reserved.
// Project Apex: Universal JIT-Accelerated Vector Engine (10B+ RPS)
//
// This work is licensed under the Business Source License 1.1 until 2029-05-03,
// transitioning to the Apache License 2.0 thereafter.
// See the LICENSE file in the repository root for the full text.

#include "apex/jit/circuit_library.hpp"

namespace apex {
namespace jit {

// ULL-Compliant: Zero heap, branchless bitwise logic, constant time
// Latency target: ~3ns per bit-slice compare (ARM64 bic/and/orr chain)
void CircuitLibrary::emit_gt_64(
    asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& a_bit,
    const asmjit::a64::Gp& b_bit,
    asmjit::a64::Gp& gt_mask,
    asmjit::a64::Gp& eq_mask) noexcept {
    using namespace asmjit::a64;

    // Branchless bit-sliced GT comparison:
    // GT = GT | (EQ & A_bit & ~B_bit)
    // EQ = EQ & ~(A_bit ^ B_bit)

    // Use x6 and x7 as scratch registers (free in Phase 2)
    Gp temp = x6;
    Gp eq_and_temp = x7;

    // temp = A_bit & ~B_bit (using BIC: Bit Clear)
    a.bic(temp, a_bit, b_bit);

    // eq_and_temp = EQ & temp
    a.and_(eq_and_temp, eq_mask, temp);

    // GT_new = GT | (EQ & temp)
    a.orr(gt_mask, gt_mask, eq_and_temp);

    // temp = A_bit ^ B_bit (reuse temp register)
    a.eor(temp, a_bit, b_bit);

    // EQ_new = EQ & ~(A_bit ^ B_bit)
    a.bic(eq_mask, eq_mask, temp);
}

// ULL-Compliant: Zero heap, branchless bit-mux, constant time
// Latency target: ~2ns per mux operation (AND/BIC/ORR chain)
void CircuitLibrary::emit_mux(
    asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& cond,
    const asmjit::a64::Gp& a_mask,
    const asmjit::a64::Gp& b_mask,
    asmjit::a64::Gp& out,
    const asmjit::a64::Gp& tmp) noexcept {
    using namespace asmjit::a64;

    // MUX: out = (cond & a_mask) | (~cond & b_mask)
    // tmp1 = cond & a_mask
    a.and_(tmp, cond, a_mask);
    // tmp2 = ~cond & b_mask = BIC(b_mask, cond)
    a.bic(out, b_mask, cond);
    // result = tmp | tmp2
    a.orr(out, tmp, out);
}

} // namespace jit
} // namespace apex
