# 4. JIT Compiler Architecture & Logic Synthesis

AarchGate does not execute queries using a virtual machine or an interpreter. Instead, it utilizes **Just-In-Time (JIT) Compilation** to synthesize raw ARM64 machine code that represents the "logical circuit" of the query. By compiling dynamic expressions into static machine code at runtime, AarchGate eliminates the overhead of instruction fetching and branch-dependent interpretation [#Neumann2011].

## 4.1 AsmJit and Runtime Emission

AarchGate integrates **AsmJit**, a complete C++ library for machine code generation. AsmJit provides an `a64::Assembler` that allows the engine to emit native ARM64 instructions directly into an executable memory buffer.

The compilation process proceeds in three phases:
1.  **AST Flattening**: The engine parses the logical expression (e.g., `price > 25000`) into a flattened Abstract Syntax Tree.
2.  **Register Allocation**: Each bit-plane and intermediate result is mapped to one of the 31 general-purpose registers (`x0-x30`).
3.  **Instruction Emission**: The engine emits an unrolled loop of assembly instructions.

## 4.2 Branchless Ripple-Carry Logic

The fundamental innovation in AarchGate's JIT is the use of **Ripple-Carry Comparison.** Standard CPU comparisons require a branch (`B.GT`). In a bit-sliced world, AarchGate evaluates all 64 rows simultaneously by simulating a binary comparator circuit using bitwise instructions.

To compare a bit-sliced field $A$ against a constant $K$, the JIT maintains two 64-bit state registers:
*   `GT_mask` ($G$): Tracks rows where $A > K$ is definitely true.
*   `EQ_mask` ($E$): Tracks rows where $A$ and $K$ are currently equal.

Starting from the Most Significant Bit (bit 63) down to bit 0, for each bit $i$, the logic is:
If $K_i = 0$:
$$G = G \ | \ (E \ \& \ A_i)$$
$$E = E \ \& \ (\sim A_i)$$
If $K_i = 1$:
$$E = E \ \& \ A_i$$

By the end of 64 iterations, the `GT_mask` contains a `1` bit for every row that satisfied the condition. This entire process involves **zero branching.**

```asm
; Listing 4.1: Emitted ARM64 ASM for Bit-Plane i (where Ki = 0)
; x9  = GT_mask
; x10 = EQ_mask
; x11 = current bit_plane (loaded from memory)

AND  x12, x10, x11      ; temp = EQ & A_i
ORR  x9,  x9,  x12      ; GT |= temp
BIC  x10, x10, x11      ; EQ &= ~A_i (Branchless Update)
```

## 4.3 ISA-Level Optimizations

### 4.3.1 Immediate Pointer Encoding
ARM64 cannot load a 64-bit pointer (the address of a bit-plane buffer) in a single instruction. AarchGate's JIT uses a sequence of `MOVZ` (Move Zero) and `MOVK` (Move Keep) instructions to "stitch" the pointer together in 16-bit segments.

```asm
MOVZ x16, #0xABCD, LSL #0
MOVK x16, #0xEF01, LSL #16
MOVK x16, #0x2345, LSL #32
MOVK x16, #0x6789, LSL #48 ; x16 now holds a 64-bit address
```

### 4.3.2 Post-Indexed Memory Access
To minimize instruction count inside hot loops, AarchGate utilizes ARM64’s **Post-Indexed Addressing.** A single `LDR` instruction can load a bit-plane and decrement the base pointer in the same cycle, preparing for the next iteration.

```asm
LDR x11, [x16], #-8    ; Load bit_plane and x16 -= 8
```

## 4.4 Adaptive Bit-Width Pruning

If a dataset only contains values up to $2^{16}$, processing all 64 bit-planes is wasteful. The AarchGate JIT analyzes the metadata of the `ColumnBuffer` and **shortens the circuit.** If the maximum value is $V$, the JIT emits only $log_2(V)$ iterations. This "Pruning" technique provides an instant $4\times$ speedup for 16-bit data compared to 64-bit data.

## 4.5 Execution Integrity and Barriers

Because the machine code is synthesized at runtime, C++ compilers and CPU out-of-order engines can sometimes incorrectly "memoize" or reorder execution. AarchGate uses **Volatile Function Pointers** and explicit `__asm__ volatile` memory barriers to ensure that every call to the JIT kernel is fresh and that the CPU does not skip the logical evaluation.

![Figure 4: Ripple-Carry Logic Transition Diagram](../figures/ripple_carry.png)
