// (c) 2024-2026 Suprath PS. All rights reserved.
// Project Apex: Universal JIT-Accelerated Vector Engine (10B+ RPS)
//
// This work is licensed under the Business Source License 1.1 until 2029-05-03,
// transitioning to the Apache License 2.0 thereafter.
// See the LICENSE file in the repository root for the full text.

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "compute/bit_slicer.cpp"
#include "hwy/foreach_target.h"

#include "apex/compute/bit_slicer.hpp"
#include "apex/compute/column_buffer.hpp"
#include "hwy/highway.h"
#include <cstring>

HWY_BEFORE_NAMESPACE();
namespace apex::compute {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// ULL-Compliant: Zero heap, branchless SIMD, cache-aligned
// Latency target: ~12ns per stage
// Stage 5: stride=32, shift=32
HWY_INLINE void Stage5(uint64_t* HWY_RESTRICT A) {
    const hn::CappedTag<uint64_t, 32> d;
    const size_t N = hn::Lanes(d);
    const auto vmask = hn::Set(d, 0x00000000FFFFFFFFULL);

    for (int k = 0; k < 32; k += static_cast<int>(N)) {
        const auto va = hn::Load(d, A + k);
        const auto vb = hn::Load(d, A + k + 32);
        const auto vt = hn::And(hn::Xor(hn::ShiftRight<32>(va), vb), vmask);
        hn::Store(hn::Xor(vb, vt), d, A + k + 32);
        hn::Store(hn::Xor(va, hn::ShiftLeft<32>(vt)), d, A + k);
    }
}

// ULL-Compliant: Zero heap, branchless SIMD, vectorized
// Stage 4: stride=16, shift=16
HWY_INLINE void Stage4(uint64_t* HWY_RESTRICT A) {
    const hn::CappedTag<uint64_t, 16> d;
    const size_t N = hn::Lanes(d);
    const auto vmask = hn::Set(d, 0x0000FFFF0000FFFFULL);

    for (int block = 0; block < 64; block += 32) {
        for (int k = block; k < block + 16; k += static_cast<int>(N)) {
            const auto va = hn::Load(d, A + k);
            const auto vb = hn::Load(d, A + k + 16);
            const auto vt = hn::And(hn::Xor(hn::ShiftRight<16>(va), vb), vmask);
            hn::Store(hn::Xor(vb, vt), d, A + k + 16);
            hn::Store(hn::Xor(va, hn::ShiftLeft<16>(vt)), d, A + k);
        }
    }
}

// ULL-Compliant: Zero heap, branchless SIMD, vectorized
// Stage 3: stride=8, shift=8
HWY_INLINE void Stage3(uint64_t* HWY_RESTRICT A) {
    const hn::CappedTag<uint64_t, 8> d;
    const size_t N = hn::Lanes(d);
    const auto vmask = hn::Set(d, 0x00FF00FF00FF00FFULL);

    for (int block = 0; block < 64; block += 16) {
        for (int k = block; k < block + 8; k += static_cast<int>(N)) {
            const auto va = hn::Load(d, A + k);
            const auto vb = hn::Load(d, A + k + 8);
            const auto vt = hn::And(hn::Xor(hn::ShiftRight<8>(va), vb), vmask);
            hn::Store(hn::Xor(vb, vt), d, A + k + 8);
            hn::Store(hn::Xor(va, hn::ShiftLeft<8>(vt)), d, A + k);
        }
    }
}

// ULL-Compliant: Zero heap, branchless SIMD, vectorized
// Stage 2: stride=4, shift=4
HWY_INLINE void Stage2(uint64_t* HWY_RESTRICT A) {
    const hn::CappedTag<uint64_t, 4> d;
    const size_t N = hn::Lanes(d);
    const auto vmask = hn::Set(d, 0x0F0F0F0F0F0F0F0FULL);

    for (int block = 0; block < 64; block += 8) {
        for (int k = block; k < block + 4; k += static_cast<int>(N)) {
            const auto va = hn::Load(d, A + k);
            const auto vb = hn::Load(d, A + k + 4);
            const auto vt = hn::And(hn::Xor(hn::ShiftRight<4>(va), vb), vmask);
            hn::Store(hn::Xor(vb, vt), d, A + k + 4);
            hn::Store(hn::Xor(va, hn::ShiftLeft<4>(vt)), d, A + k);
        }
    }
}

// ULL-Compliant: Zero heap, branchless SIMD, vectorized
// Stage 1: stride=2, shift=2
HWY_INLINE void Stage1(uint64_t* HWY_RESTRICT A) {
    const hn::CappedTag<uint64_t, 2> d;
    const size_t N = hn::Lanes(d);
    const auto vmask = hn::Set(d, 0x3333333333333333ULL);

    for (int block = 0; block < 64; block += 4) {
        for (int k = block; k < block + 2; k += static_cast<int>(N)) {
            const auto va = hn::Load(d, A + k);
            const auto vb = hn::Load(d, A + k + 2);
            const auto vt = hn::And(hn::Xor(hn::ShiftRight<2>(va), vb), vmask);
            hn::Store(hn::Xor(vb, vt), d, A + k + 2);
            hn::Store(hn::Xor(va, hn::ShiftLeft<2>(vt)), d, A + k);
        }
    }
}

// ULL-Compliant: Zero heap, branchless scalar loop-unrolled
// Scalar Stage 0: stride=1 (cross-lane dependency prevents vectorization)
HWY_INLINE void Stage0(uint64_t* HWY_RESTRICT A) {
    constexpr uint64_t kMask0 = 0x5555555555555555ULL;
    for (int i = 0; i < 64; i += 2) {
        uint64_t t = ((A[i] >> 1) ^ A[i + 1]) & kMask0;
        A[i + 1] ^= t;
        A[i] ^= t << 1;
    }
}

// ULL-Compliant: Zero heap, branchless, 6-stage transpose pipeline
// Latency target: ~80ns total per 64x64 matrix
HWY_INLINE void Transpose64x64(uint64_t* HWY_RESTRICT A) {
    Stage5(A);
    Stage4(A);
    Stage3(A);
    Stage2(A);
    Stage1(A);
    Stage0(A);
}

// ULL-Compliant: Zero heap, zero-copy in-place transpose, cache-aligned
// Latency target: ~80ns per 64-row column
HWY_INLINE void Slice_impl(const ColumnBuffer& in, ColumnBuffer& out) {
    std::memcpy(out.data, in.data, sizeof(ColumnBuffer));
    Transpose64x64(out.data);
}

} // namespace HWY_NAMESPACE
} // namespace apex::compute
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace apex::compute {
HWY_EXPORT(Slice_impl);

void BitSlicer::slice(const ColumnBuffer& in, ColumnBuffer& out) noexcept {
    HWY_DYNAMIC_DISPATCH(Slice_impl)(in, out);
}

} // namespace apex::compute
#endif // HWY_ONCE
