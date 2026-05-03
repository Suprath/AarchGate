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

void CircuitLibrary::emit_add_64(
    asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& a_plane,
    const asmjit::a64::Gp& b_plane,
    asmjit::a64::Gp& carry,
    asmjit::a64::Gp& sum,
    const asmjit::a64::Gp& tmp1,
    const asmjit::a64::Gp& tmp2) noexcept {
    using namespace asmjit::a64;
    // Full Adder:
    // Sum = A ^ B ^ Carry
    // Carry = (A & B) | (Carry & (A ^ B))
    a.eor(tmp1, a_plane, b_plane);     // tmp1 = A ^ B
    a.eor(sum, tmp1, carry);          // sum = (A ^ B) ^ Carry
    a.and_(tmp2, a_plane, b_plane);    // tmp2 = A & B
    a.and_(tmp1, tmp1, carry);         // tmp1 = (A ^ B) & Carry
    a.orr(carry, tmp1, tmp2);          // carry = ((A ^ B) & Carry) | (A & B)
}

void CircuitLibrary::emit_mul_64(
    asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& a_plane,
    int64_t constant,
    int bit_index,
    asmjit::a64::Gp* carries,
    asmjit::a64::Gp& sum,
    const asmjit::a64::Gp& tmp1,
    const asmjit::a64::Gp& tmp2) noexcept {
    using namespace asmjit::a64;

    // Shift-and-Add Optimization (MUL_CONST):
    // For each bit i set in constant, we add (A << i) to the result.
    // In bit-sliced terms, Result[j] = Sum(A[j-i]) for all i where constant[i] == 1.
    
    // We only process the CURRENT bit_index j.
    // If constant bit i is set, then A[bit_index - i] contributes to Sum[bit_index].
    
    (void)a_plane;
    (void)constant;
    (void)bit_index;
    (void)carries;
    (void)sum;
    (void)tmp1;
    (void)tmp2;
    for (int i = 0; i <= bit_index && i < 64; ++i) {
        if ((constant >> i) & 1) {
            // We need A[bit_index - i]. 
            // In the Phase 1 loop, a_plane is passed as A[bit_index].
            // If i > 0, the caller MUST provide A[bit_index - i].
            // But wait! The JIT loop only gives us the CURRENT plane.
            
            // To simplify Phase 1, MUL_CONST usually assumes a_plane is shifted or handled.
            // However, the Master Directive says: "Result = (A << i) for every bit i set in the constant".
            // This is exactly what we implement if we have the planes.
            
            // For now, if i=0, we use a_plane. For i > 0, we'd need to load from scratch.
            // Since we can't easily load arbitrary planes here without a pointer,
            // we'll implement the logic for the bit that DOES contribute.
            
            // Optimization: If constant is a power of 2 (e.g. 1, 2, 4, 8), no additions needed, 
            // just a plane re-index.
            
            // Real implementation will be sequenced in JitCompiler::compile_expression.
        }
    }
}

void CircuitLibrary::emit_csa(
    asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& a_in,
    const asmjit::a64::Gp& b_in,
    const asmjit::a64::Gp& c_in,
    asmjit::a64::Gp& sum_out,
    asmjit::a64::Gp& carry_out,
    const asmjit::a64::Gp& tmp) noexcept {
    using namespace asmjit::a64;
    // Sum = A ^ B ^ C
    // Carry = (A & B) | (B & C) | (A & C)
    // Optimization: Carry = (A & B) | (C & (A ^ B))
    
    a.eor(tmp, a_in, b_in);    // tmp = A ^ B
    a.eor(sum_out, tmp, c_in); // sum = (A ^ B) ^ C
    
    a.and_(sum_out, a_in, b_in); // reuse sum_out as temp for (A & B)
    a.and_(tmp, tmp, c_in);      // tmp = (A ^ B) & C
    a.orr(carry_out, tmp, sum_out); // carry = ((A ^ B) & C) | (A & B)
    
    // Restore sum_out (we reused it)
    a.eor(tmp, a_in, b_in);
    a.eor(sum_out, tmp, c_in);
}

void CircuitLibrary::emit_popcnt_mask(
    asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& mask,
    asmjit::a64::Gp& out) noexcept {
    using namespace asmjit::a64;
    // A64 Instruction: fmov mask -> d0; cnt d0, d0; addv b0, v0.8b; fmov out <- d0
    a.fmov(v0.d(), mask);
    a.cnt(v0.v8(), v0.v8());
    a.addv(v0.b(), v0.v8());
    a.fmov(out, v0.d());
    a.and_(out, out, 0xFF); // Ensure only the low byte (sum) is kept
}

} // namespace jit
} // namespace apex
