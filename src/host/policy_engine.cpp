// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: JIT-Accelerated Sandbox Policy Engine

#include "host/policy_engine.hpp"
#include "apex/jit/ir.hpp"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace aarchgate {

PolicyEngine::PolicyEngine() {
    init_JIT_schema();
}

void PolicyEngine::init_JIT_schema() {
    // 1. Define schema fields matching SyscallTraceRecord layout
    std::vector<apex::core::FieldDescriptor> fields = {
        {"pid",             (uint32_t)offsetof(SyscallTraceRecord, pid),             64, apex::core::DataType::UINT64},
        {"ppid",            (uint32_t)offsetof(SyscallTraceRecord, ppid),            64, apex::core::DataType::UINT64},
        {"event_type",      (uint32_t)offsetof(SyscallTraceRecord, event_type),      64, apex::core::DataType::UINT64},
        {"is_preinstall",   (uint32_t)offsetof(SyscallTraceRecord, is_preinstall),   64, apex::core::DataType::UINT64},
        {"is_sensitive",    (uint32_t)offsetof(SyscallTraceRecord, is_sensitive),    64, apex::core::DataType::UINT64},
        {"is_unauthorized", (uint32_t)offsetof(SyscallTraceRecord, is_unauthorized), 64, apex::core::DataType::UINT64}
    };

    // Stride is exactly 64 bytes
    JIT_engine_.register_schema("SyscallTrace", fields, sizeof(SyscallTraceRecord));

    // 2. Build JIT Policy AST:
    // policy_violation = is_preinstall & ((event_type == 2 /* OPEN */ & is_sensitive) | (event_type == 3 /* CONNECT */ & is_unauthorized))
    using namespace apex::builder;

    auto is_preinstall   = Load("is_preinstall");
    auto event_type      = Load("event_type");
    auto is_sensitive    = Load("is_sensitive");
    auto is_unauthorized = Load("is_unauthorized");

    // Convert fields to BITMASKs by comparing them to Const(1)
    auto is_preinstall_mask   = EQ(is_preinstall, Const(1));
    auto is_sensitive_mask    = EQ(is_sensitive, Const(1));
    auto is_unauthorized_mask = EQ(is_unauthorized, Const(1));

    // Open policy violation check: event_type == EVENT_OPEN (2) AND accessing sensitive path
    auto open_check = And(EQ(event_type, Const(EVENT_OPEN)), is_sensitive_mask);

    // Connect policy violation check: event_type == EVENT_CONNECT (3) AND unauthorized connection
    auto connect_check = And(EQ(event_type, Const(EVENT_CONNECT)), is_unauthorized_mask);

    // Any violation condition
    auto violation_check = Or(open_check, connect_check);

    // Root node: must be spawned under preinstall/lifecycle script AND violate policy
    auto root = And(is_preinstall_mask, violation_check);

    // Compile the AST to native ARM64 machine code
    JIT_engine_.set_expression("SyscallTrace", root, apex::ExecutionMode::BIT_SLICED);
}

SyscallTraceRecord PolicyEngine::process_event(const SyscallEvent& event) {
    SyscallTraceRecord rec{};
    std::memset(&rec, 0, sizeof(rec));

    rec.pid = event.pid;
    rec.ppid = event.ppid;
    rec.event_type = event.event_type;

    std::string comm(event.comm);
    std::string arg(event.arg_str);

    // 1. Process Tree Tracking & Preinstall Inheritance
    bool is_preinstall_process = false;
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);

        // Check if parent process is registered and flagged as preinstall
        auto parent_it = process_tree_.find(event.ppid);
        bool parent_is_untrusted = (parent_it != process_tree_.end()) && parent_it->second.is_preinstall_descendant;

        if (event.event_type == EVENT_EXEC) {
            // Determine if this new process is an untrusted lifecycle script
            bool is_untrusted_script = parent_is_untrusted;

            // If the parent is the trusted package manager (e.g. npm, node, yarn, pnpm)
            // but the child being run is a shell (/bin/sh, /bin/bash) or other binary,
            // it is a lifecycle script entry point.
            if (!is_untrusted_script && (comm == "sh" || comm == "bash" || comm == "node" || comm == "python")) {
                // If spawned by the package manager, it's untrusted
                // Standard detection: check parent name if available (or assume any child of host npm VM execution is untrusted except the guest agent itself)
                // In guest sandbox VM, the guest daemon itself runs as PID 1/2 or systemd. npm is PID > 10.
                // Any child spawned by npm (except npm itself) is untrusted.
                // We'll mark it as untrusted.
                if (event.ppid > 1) { // Assume spawned by npm installer
                    is_untrusted_script = true;
                }
            }

            process_tree_[event.pid] = ProcessNode{event.pid, event.ppid, comm, is_untrusted_script};
            is_preinstall_process = is_untrusted_script;
        } else {
            // For OPEN and CONNECT, look up the process's preinstall status
            auto proc_it = process_tree_.find(event.pid);
            if (proc_it != process_tree_.end()) {
                is_preinstall_process = proc_it->second.is_preinstall_descendant;
            } else {
                // Fallback to parent state if not in map
                is_preinstall_process = parent_is_untrusted;
            }
        }
    }

    rec.is_preinstall = is_preinstall_process ? 1 : 0;

    // 2. Policy Classification
    if (event.event_type == EVENT_OPEN) {
        // Classify sensitive paths
        // Match home-level credentials or config files (.ssh, .aws, .env, .npmrc, .gitconfig)
        bool has_sensitive_pattern = false;
        if (arg.find(".ssh/") != std::string::npos ||
            arg.find(".aws/") != std::string::npos ||
            arg.find("aws/credentials") != std::string::npos ||
            arg.find(".env") != std::string::npos ||
            arg.find(".npmrc") != std::string::npos ||
            arg.find(".gitconfig") != std::string::npos) {
            has_sensitive_pattern = true;
        }
        rec.is_sensitive = has_sensitive_pattern ? 1 : 0;
    } 
    else if (event.event_type == EVENT_CONNECT) {
        // Zero-Trust Outbound Rule: Preinstall scripts have NO business connecting to the internet.
        // Node dependencies are fetched by npm itself, not by install scripts.
        // Therefore, any network connection from a preinstall descendant is flagged as unauthorized.
        rec.is_unauthorized = 1;
    }

    return rec;
}

uint64_t PolicyEngine::evaluate_batch(const SyscallTraceRecord* records, size_t count) {
    if (count == 0) return 0;

    // AarchGate JIT execute runs on row counts.
    // If count < 64, we can evaluate it, but to run in high-performance native bit-sliced mode,
    // the ApexEngine expects multiples of 64 or pads them.
    // Let's call execute, which slices and runs the JIT compiled expression.
    return JIT_engine_.execute(records, count);
}

bool PolicyEngine::is_preinstall_process(uint64_t pid) {
    std::lock_guard<std::mutex> lock(tree_mutex_);
    auto it = process_tree_.find(pid);
    return (it != process_tree_.end()) && it->second.is_preinstall_descendant;
}

void PolicyEngine::register_process(uint64_t pid, uint64_t ppid, const std::string& comm, bool force_preinstall) {
    std::lock_guard<std::mutex> lock(tree_mutex_);
    process_tree_[pid] = ProcessNode{pid, ppid, comm, force_preinstall};
}

void PolicyEngine::unregister_process(uint64_t pid) {
    std::lock_guard<std::mutex> lock(tree_mutex_);
    process_tree_.erase(pid);
}

} // namespace aarchgate
