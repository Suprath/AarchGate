#include <iostream>
#include <vector>
#include <csignal>
#include <atomic>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <linux/vm_sockets.h>
#include <bpf/libbpf.h>
#include "vsock_protocol.h"
#include "tracer.skel.h"

using namespace aarchgate;

// Define MTE constants if they are not present in older Linux header files
#ifndef PR_SET_TAGGED_ADDR_CTRL
#define PR_SET_TAGGED_ADDR_CTRL 55
#define PR_TAGGED_ADDR_ENABLE (1UL << 0)
#define PR_MTE_TCF_SYNC (1UL << 1)
#define PR_MTE_TAG_SHIFT 3
#endif

// Global state variables
std::atomic<bool> g_stop{false};
int g_vsock_fd = -1;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop = true;
    }
}

// Configures ARMv9 Hardware Memory Tagging (MTE) for execution safety
void configure_mte() {
#ifdef __aarch64__
    // Enable MTE in Synchronous Mode (instant trap on memory out-of-bounds)
    unsigned long mask = PR_TAGGED_ADDR_ENABLE | PR_MTE_TCF_SYNC | (0xfffe << PR_MTE_TAG_SHIFT);
    if (prctl(PR_SET_TAGGED_ADDR_CTRL, mask, 0, 0, 0) == 0) {
        std::cout << "[AarchGate Guest Agent] ARMv9 MTE Hardware Isolation active (TCF=SYNC)." << std::endl;
    } else {
        std::cerr << "[AarchGate Guest Agent] WARNING: MTE initialization failed. Hardware not present." << std::endl;
    }
#else
    std::cout << "[AarchGate Guest Agent] MTE not supported (requires arm64 architecture)." << std::endl;
#endif
}

// Establish VSOCK telemetry stream to Host (always CID 2)
int connect_to_host() {
    int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "[AarchGate Guest Agent] Failed to open AF_VSOCK socket." << std::endl;
        return -1;
    }

    struct sockaddr_vm addr{};
    addr.svm_family = AF_VSOCK;
    addr.svm_port = VSOCK_TRACE_PORT;
    addr.svm_cid = 2; // Host CID is statically 2 under Apple Virtualization.framework

    std::cout << "[AarchGate Guest Agent] Connecting to Host CID 2 Port " << VSOCK_TRACE_PORT << "..." << std::endl;
    
    // Retry connection a few times in case VM boots faster than host binds
    int retries = 5;
    while (retries-- > 0) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            std::cout << "[AarchGate Guest Agent] Telemetry socket connected to Host." << std::endl;
            return fd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cerr << "[AarchGate Guest Agent] ERROR: Failed to connect to Host." << std::endl;
    close(fd);
    return -1;
}

// Callback invoked when eBPF ring buffer gathers a new syscall trace
static int handle_bpf_event(void *ctx, void *data, size_t size) {
    if (g_vsock_fd == -1) return 0;

    // Send the packed binary telemetry structure directly over VSOCK
    ssize_t bytes_written = write(g_vsock_fd, data, size);
    if (bytes_written < 0) {
        std::cerr << "[AarchGate Guest Agent] VSOCK write failed. Host closed connection?" << std::endl;
        g_stop = true;
    }
    return 0;
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "                 AarchGate Guest Agent v1.0                      \n";
    std::cout << "=================================================================\n" << std::endl;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 1. Hardware memory coloring
    configure_mte();

    // 2. Establish telemetry pipe to Host Control Plane
    g_vsock_fd = connect_to_host();
    if (g_vsock_fd < 0) {
        return 1;
    }

    // 3. Load eBPF programs via skeleton
    struct tracer_bpf* skel = nullptr;
    skel = tracer_bpf__open_and_load();
    if (!skel) {
        std::cerr << "[AarchGate Guest Agent] ERROR: Failed to load eBPF tracer skeleton." << std::endl;
        close(g_vsock_fd);
        return 1;
    }

    // Attach hooks to execve, openat, and connect syscall tracepoints
    int err = tracer_bpf__attach(skel);
    if (err) {
        std::cerr << "[AarchGate Guest Agent] ERROR: Failed to attach eBPF hooks." << std::endl;
        tracer_bpf__destroy(skel);
        close(g_vsock_fd);
        return 1;
    }

    // 4. Initialize Ring Buffer reader
    struct ring_buffer* rb = nullptr;
    rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_bpf_event, nullptr, nullptr);
    if (!rb) {
        std::cerr << "[AarchGate Guest Agent] ERROR: Failed to create BPF ring buffer." << std::endl;
        tracer_bpf__destroy(skel);
        close(g_vsock_fd);
        return 1;
    }

    std::cout << "[AarchGate Guest Agent] Telemetry tracer is active. Streaming syscall events..." << std::endl;

    // Polling loop
    while (!g_stop) {
        err = ring_buffer__poll(rb, 100 /* timeout ms */);
        if (err < 0 && err != -EINTR) {
            std::cerr << "[AarchGate Guest Agent] Error polling ring buffer: " << err << std::endl;
            break;
        }
    }

    // 5. Cleanup
    std::cout << "[AarchGate Guest Agent] Shutting down agent..." << std::endl;
    ring_buffer__free(rb);
    tracer_bpf__destroy(skel);
    if (g_vsock_fd != -1) {
        close(g_vsock_fd);
    }

    std::cout << "[AarchGate Guest Agent] Guest agent exited." << std::endl;
    return 0;
}
