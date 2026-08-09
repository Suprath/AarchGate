// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: Monitor IPC Protocol

#pragma once

#include <cstdint>

#ifndef EVENT_EXEC
#define EVENT_EXEC 1
#define EVENT_OPEN 2
#define EVENT_CONNECT 3
#endif

#define EVENT_MTE_TRAP 4
#define EVENT_VM_STATE_CHANGE 5

namespace aarchgate {

struct alignas(64) MonitorEvent {
    uint64_t timestamp_ns;
    uint64_t pid;
    uint64_t ppid;
    uint64_t event_type; // 1=EXEC, 2=OPEN, 3=CONNECT, 4=MTE_TRAP, 5=VM_STATE_CHANGE
    char comm[32];
    char arg_str[256];
    uint32_t ip_address;
    uint16_t port;
    
    // Sandbox specific classifications
    uint8_t is_preinstall;
    uint8_t is_sensitive;
    uint8_t is_unauthorized;
    uint8_t vm_running; // 1 if VM is running, 0 if stopped/killed
    uint8_t padding[4];
};

} // namespace aarchgate
