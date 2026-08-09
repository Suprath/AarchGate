// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: Zero-Trust npm Sandbox Telemetry Protocol
//
// Shared types and structures for communication between the macOS Host Daemon
// and the Linux Guest VM Agent.

#pragma once
#include <cstdint>

namespace aarchgate {

// Port configuration for virtio-vsock
constexpr uint32_t VSOCK_TRACE_PORT = 10245;
constexpr uint32_t VSOCK_SSH_PORT = 10246;

// Syscall event categories monitored by eBPF
enum EventType : uint32_t {
    EVENT_EXEC = 1,
    EVENT_OPEN = 2,
    EVENT_CONNECT = 3
};

// Binary packet structure streamed from the guest to the host.
// Must be packed to ensure byte-perfect alignment between macOS Clang and Linux GCC.
// WARNING: This must exactly match struct syscall_event_t in src/guest/tracer.bpf.c
struct SyscallEvent {
    uint64_t timestamp_ns;
    uint64_t pid;
    uint64_t ppid;
    uint64_t event_type;       // EventType (EXEC=1, OPEN=2, CONNECT=3) — __u64 to match eBPF struct
    char comm[32];              // Comm (process short name) — 32 bytes to match bpf_get_current_comm
    char arg_str[256];          // Filepath or execve arguments
    uint32_t ip_address;        // IPv4 address for EVENT_CONNECT
    uint16_t port;              // Port for EVENT_CONNECT
    uint8_t padding[2];         // Padding to match guest struct
} __attribute__((packed));

} // namespace aarchgate
