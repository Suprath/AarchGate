// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: guest eBPF Tracer (Linux Kernel Hook)

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>

// Matches SyscallEvent in vsock_protocol.h
struct syscall_event_t {
    __u64 timestamp_ns;
    __u64 pid;
    __u64 ppid;
    __u64 event_type;
    char comm[32];
    char arg_str[256];
    __u32 ip_address;
    __u16 port;
    __u8 padding[2];
} __attribute__((packed));

// Telemetry events values
#define EVENT_EXEC 1
#define EVENT_OPEN 2
#define EVENT_CONNECT 3

// BPF Ring Buffer for lockless user-space streaming
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024); // 256KB buffer
} rb SEC(".maps");

char LICENSE[] SEC("license") = "Dual BSD/GPL";

// Helper to fill common event fields
static __always_inline void fill_common(struct syscall_event_t* ev, __u64 type) {
    ev->timestamp_ns = bpf_ktime_get_ns();
    
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    ev->pid = pid_tgid >> 32; // Extract PID (TGID in kernel-speak)
    ev->event_type = type;
    
    bpf_get_current_comm(&ev->comm, sizeof(ev->comm));

    // Resolve PPID by reading the parent task structure
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    struct task_struct *parent = BPF_CORE_READ(task, real_parent);
    if (parent) {
        ev->ppid = BPF_CORE_READ(parent, tgid);
    } else {
        ev->ppid = 0;
    }
}

// 1. Hook Execve to track process execution and inherit sandbox context
SEC("tracepoint/syscalls/sys_enter_execve")
int trace_execve(struct trace_event_raw_sys_enter* ctx) {
    struct syscall_event_t* ev;

    ev = bpf_ringbuf_reserve(&rb, sizeof(*ev), 0);
    if (!ev) return 0;

    fill_common(ev, EVENT_EXEC);

    // Read the executable path (first argument of execve)
    const char* filename = (const char*)ctx->args[0];
    bpf_probe_read_user_str(ev->arg_str, sizeof(ev->arg_str), filename);

    bpf_ringbuf_submit(ev, 0);
    return 0;
}

// 2. Hook Openat to catch sensitive file read attempts (.ssh/id_rsa, .env)
SEC("tracepoint/syscalls/sys_enter_openat")
int trace_openat(struct trace_event_raw_sys_enter* ctx) {
    struct syscall_event_t* ev;

    ev = bpf_ringbuf_reserve(&rb, sizeof(*ev), 0);
    if (!ev) return 0;

    fill_common(ev, EVENT_OPEN);

    // Read the filename path (second argument of openat)
    const char* filename = (const char*)ctx->args[1];
    bpf_probe_read_user_str(ev->arg_str, sizeof(ev->arg_str), filename);

    bpf_ringbuf_submit(ev, 0);
    return 0;
}

// 3. Hook Connect to intercept reverse shells and token exfiltration sockets
SEC("tracepoint/syscalls/sys_enter_connect")
int trace_connect(struct trace_event_raw_sys_enter* ctx) {
    struct syscall_event_t* ev;

    ev = bpf_ringbuf_reserve(&rb, sizeof(*ev), 0);
    if (!ev) return 0;

    fill_common(ev, EVENT_CONNECT);

    // Read the sockaddr structure (second argument of connect)
    struct sockaddr* addr = (struct sockaddr*)ctx->args[1];
    if (addr) {
        short family = 0;
        bpf_probe_read_kernel(&family, sizeof(family), &addr->sa_family);

        if (family == AF_INET) { // IPv4
            struct sockaddr_in addr_in;
            bpf_probe_read_user(&addr_in, sizeof(addr_in), addr);

            ev->ip_address = addr_in.sin_addr.s_addr;
            
            // Convert network byte order (big endian) port to host byte order
            __u16 raw_port = addr_in.sin_port;
            ev->port = ((raw_port & 0xff) << 8) | ((raw_port & 0xff00) >> 8);
            __builtin_memcpy(ev->arg_str, "ipv4_connect", 13); // Mark connect type
        } else if (family == AF_INET6) { // IPv6
            __builtin_memcpy(ev->arg_str, "ipv6_connect", 13);
        } else {
            __builtin_memcpy(ev->arg_str, "other_connect", 14);
        }
    }

    bpf_ringbuf_submit(ev, 0);
    return 0;
}
