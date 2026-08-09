// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: JIT-Accelerated Sandbox Policy Engine

#pragma once

#include "apex/AarchGate.hpp"
#include "apex/jit/ir.hpp"
#include "common/vsock_protocol.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>

namespace aarchgate {

// Row format aligned to 64 bytes (L1 cache line) to prevent false sharing
// and enable fast transposition to bit-planes.
struct alignas(64) SyscallTraceRecord {
    uint64_t pid;
    uint64_t ppid;
    uint64_t event_type;       // 1 = EXEC, 2 = OPEN, 3 = CONNECT
    uint64_t is_preinstall;     // 1 or 0 (flagged if descendant of untrusted script)
    uint64_t is_sensitive;      // 1 or 0 (path contains .ssh, .env, etc.)
    uint64_t is_unauthorized;   // 1 or 0 (outbound connection to unauthorized IP)
    uint64_t padding[2];        // Padding to make exactly 64 bytes (6 * 8 + 16 = 64)
};

struct ProcessNode {
    uint64_t pid;
    uint64_t ppid;
    std::string comm;
    bool is_preinstall_descendant;
};

class PolicyEngine {
public:
    PolicyEngine();
    ~PolicyEngine() = default;

    // Process a raw syscall event from VSOCK, updates process tree,
    // classifies paths/IPs, and returns a SyscallTraceRecord.
    SyscallTraceRecord process_event(const SyscallEvent& event);

    // Evaluates a batch of exactly 64 records using AarchGate's JIT bit-sliced engine.
    // Returns a 64-bit mask where each bit represents a violation at that record index.
    uint64_t evaluate_batch(const SyscallTraceRecord* records, size_t count);

    // Thread-safe helpers to check process tree status
    bool is_preinstall_process(uint64_t pid);
    void register_process(uint64_t pid, uint64_t ppid, const std::string& comm, bool force_preinstall = false);
    void unregister_process(uint64_t pid);

private:
    // Core AarchGate JIT engine
    apex::ApexEngine JIT_engine_;

    // Process tree tracking
    std::unordered_map<uint64_t, ProcessNode> process_tree_;
    std::mutex tree_mutex_;

    // Setup AarchGate Schema & JIT Compiled Expression
    void init_JIT_schema();
};

} // namespace aarchgate
