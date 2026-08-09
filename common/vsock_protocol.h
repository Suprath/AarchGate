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
struct SyscallEvent {
    uint64_t timestamp_ns;
    uint64_t pid;
    uint64_t ppid;
    uint32_t event_type;       // EventType (EXEC, OPEN, CONNECT)
    uint32_t is_preinstall;     // 1 if process belongs to preinstall script tree, 0 otherwise
    uint32_t is_sensitive;      // 1 if accessing sensitive path (.ssh, .env, etc.)
    uint32_t is_unauthorized;   // 1 if targeting unauthorized destination/action
    uint32_t ip_address;        // IPv4 address for EVENT_CONNECT
    uint16_t port;              // Port for EVENT_CONNECT
    char comm[16];              // Comm (process short name)
    char arg_str[256];          // Filepath or execve arguments
} __attribute__((packed));

} // namespace aarchgate
