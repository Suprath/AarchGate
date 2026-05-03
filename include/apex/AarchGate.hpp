// (c) 2024-2026 Suprath PS. All rights reserved.
// Project AarchGate: Universal JIT-Accelerated Vector Engine (10B+ RPS)
//
// This work is licensed under the Business Source License 1.1 until 2029-05-03,
// transitioning to the Apache License 2.0 thereafter.

#pragma once

#include "apex/engine.hpp"

namespace aarchgate {

// The AarchGate branded wrapper for ApexEngine
using Engine = apex::ApexEngine;

// AarchGate Native Format Descriptor
struct NativeBlock {
    alignas(64) uint64_t planes[8][64]; // Up to 8 fields, 64 planes each
};

} // namespace aarchgate
