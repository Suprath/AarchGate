// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: Sandbox Host Coordinator Daemon

#include "host/policy_engine.hpp"
#include "host/vm_controller.h"
#include "host/esf_manager.hpp"
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <iostream>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <csignal>
#include <unistd.h>
#include <pthread.h>
#include <thread>
#include <functional>
#include <cstring>
#include "common/monitor_protocol.h"

#ifdef APEX_HAS_ICEORYX
#include "iceoryx_posh/runtime/posh_runtime.hpp"
#include "iceoryx_posh/popo/publisher.hpp"
#endif

using namespace aarchgate;

// Global shutdown flag
std::atomic<bool> g_shutdown{false};
std::mutex g_cv_mutex;
std::condition_variable g_shutdown_cv;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n[AarchGate Daemon] Shutting down daemon..." << std::endl;
        g_shutdown = true;
        g_shutdown_cv.notify_all();
#if defined(__APPLE__)
        CFRunLoopStop(CFRunLoopGetMain());
#endif
    }
}

// Thread-safe batch collector for JIT Policy Evaluation
class SyscallBatchCollector {
public:
    SyscallBatchCollector(PolicyEngine& pe, VMController& vm, std::function<void(const MonitorEvent&)> cb = nullptr) 
        : pe_(pe), vm_(vm), monitor_cb_(cb) {
        // Start processing worker thread
        worker_ = std::thread(&SyscallBatchCollector::process_queue, this);
    }

    ~SyscallBatchCollector() {
        stop_ = true;
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void add_event(const SyscallEvent& event) {
        // Process process tree updates and classify immediately
        SyscallTraceRecord record = pe_.process_event(event);

        // ── Immediate Kill-Switch Fast-Path ──────────────────────────────────
        // Do NOT wait for the 64-record JIT batch. If any single event is a
        // confirmed policy violation, kill the VM right now.
        bool is_violation = (record.is_preinstall == 1) &&
                            ((record.is_sensitive == 1 && record.event_type == EVENT_OPEN) ||
                             (record.is_unauthorized == 1 && record.event_type == EVENT_CONNECT));

        if (is_violation && vm_.is_running()) {
            std::cerr << "\n" << std::string(80, '!') << "\n";
            std::cerr << "!!! CRITICAL SECURITY ALERT: POLICY VIOLATION DETECTED\n";
            std::cerr << "!!! Process '" << event.comm << "' (PID " << event.pid << ") is a preinstall script\n";
            if (record.is_sensitive) {
                std::cerr << "!!! Attempted to READ sensitive file: " << event.arg_str << "\n";
            } else {
                uint32_t ip = event.ip_address;
                std::cerr << "!!! Attempted NETWORK EXFILTRATION to: "
                          << ((ip >> 24) & 0xFF) << "." << ((ip >> 16) & 0xFF) << "."
                          << ((ip >> 8)  & 0xFF) << "." << (ip & 0xFF)
                          << ":" << event.port << "\n";
            }
            std::cerr << "!!! TERMINATING MICRO-VM SANDBOX IMMEDIATELY...\n";
            std::cerr << std::string(80, '!') << "\n" << std::endl;

            // 1. Publish the individual violation event FIRST so monitor shows it
            if (monitor_cb_) {
                MonitorEvent mon_ev{};
                mon_ev.timestamp_ns = event.timestamp_ns;
                mon_ev.pid = event.pid;
                mon_ev.ppid = event.ppid;
                mon_ev.event_type = event.event_type;
                std::strncpy(mon_ev.comm, event.comm, sizeof(mon_ev.comm));
                std::strncpy(mon_ev.arg_str, event.arg_str, sizeof(mon_ev.arg_str));
                mon_ev.ip_address = event.ip_address;
                mon_ev.port = event.port;
                mon_ev.is_preinstall = 1;
                mon_ev.is_sensitive = record.is_sensitive;
                mon_ev.is_unauthorized = record.is_unauthorized;
                mon_ev.vm_running = 1;
                monitor_cb_(mon_ev);
            }

            // 2. Then broadcast VM killed state change
            if (monitor_cb_) {
                MonitorEvent stop_ev{};
                stop_ev.event_type = EVENT_VM_STATE_CHANGE;
                std::strcpy(stop_ev.comm, "system");
                std::strcpy(stop_ev.arg_str, "Virtual Machine Terminated (Security Kill Switch)");
                stop_ev.vm_running = 0;
                monitor_cb_(stop_ev);
            }

            // 3. Stop VM on a detached thread to avoid reader-thread self-join deadlock
            std::thread([this]() {
                vm_.stop();
                g_shutdown = true;
                g_shutdown_cv.notify_all();
            }).detach();
            return; // Exit add_event immediately — VM is being terminated
        }

        if (monitor_cb_) {
            MonitorEvent mon_ev{};
            mon_ev.timestamp_ns = event.timestamp_ns;
            mon_ev.pid = event.pid;
            mon_ev.ppid = event.ppid;
            mon_ev.event_type = event.event_type;
            std::strncpy(mon_ev.comm, event.comm, sizeof(mon_ev.comm));
            std::strncpy(mon_ev.arg_str, event.arg_str, sizeof(mon_ev.arg_str));
            mon_ev.ip_address = event.ip_address;
            mon_ev.port = event.port;
            mon_ev.is_preinstall = record.is_preinstall;
            mon_ev.is_sensitive = record.is_sensitive;
            mon_ev.is_unauthorized = record.is_unauthorized;
            mon_ev.vm_running = vm_.is_running() ? 1 : 0;
            monitor_cb_(mon_ev);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        pending_records_.push_back(record);
        if (pending_records_.size() >= 64) {
            cv_.notify_one();
        }
    }

private:
    PolicyEngine& pe_;
    VMController& vm_;
    std::function<void(const MonitorEvent&)> monitor_cb_;
    std::vector<SyscallTraceRecord> pending_records_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> stop_{false};

    void process_queue() {
        std::vector<SyscallTraceRecord> eval_batch(64);

        while (!stop_) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return pending_records_.size() >= 64 || stop_;
            });

            if (stop_) break;

            // Extract exactly 64 records for JIT evaluation
            std::copy(pending_records_.begin(), pending_records_.begin() + 64, eval_batch.begin());
            pending_records_.erase(pending_records_.begin(), pending_records_.begin() + 64);
            lock.unlock();

            // Perform high-speed JIT bit-sliced policy check
            uint64_t violations = pe_.evaluate_batch(eval_batch.data(), 64);
            if (violations > 0) {
                std::cerr << "\n" << std::string(80, '!') << "\n";
                std::cerr << "!!! CRITICAL SECURITY ALERT: POLICY VIOLATION DETECTED !!!\n";
                std::cerr << "!!! A child process of a package lifecycle script attempted to access a sensitive file or network resource.\n";
                std::cerr << "!!! VIOLATION COUNT IN BATCH: " << violations << "\n";
                std::cerr << "!!! TERMINATING MICRO-VM SANDBOX TO PREVENT DATA EXFILTRATION...\n";
                std::cerr << std::string(80, '!') << "\n" << std::endl;

                // Notify monitor of VM state change
                if (monitor_cb_) {
                    MonitorEvent stop_ev{};
                    stop_ev.timestamp_ns = 0;
                    stop_ev.pid = 0;
                    stop_ev.ppid = 0;
                    stop_ev.event_type = EVENT_VM_STATE_CHANGE;
                    std::strcpy(stop_ev.comm, "system");
                    std::strcpy(stop_ev.arg_str, "Virtual Machine Terminated (Security Kill Switch)");
                    stop_ev.vm_running = 0;
                    monitor_cb_(stop_ev);
                }

                // Fire the kill switch immediately
                vm_.stop();
                
                // Trigger global shutdown
                g_shutdown = true;
                g_shutdown_cv.notify_all();
#if defined(__APPLE__)
                CFRunLoopStop(CFRunLoopGetMain());
#endif
                break;
            }
        }
    }
};

int main(int argc, char* argv[]) {
    std::cout << "=================================================================\n";
    std::cout << "                 AarchGate Sandbox Daemon v1.0                   \n";
    std::cout << "       Zero-Trust Hardware-Enforced npm Package Sandboxing       \n";
    std::cout << "=================================================================\n" << std::endl;

    // Register shutdown signals
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Command line parsing
    std::string kernel_path = "mock"; // Default to mock mode
    std::string initrd_path = "";
    std::string share_path = ".";      // Default share workspace

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--kernel" && i + 1 < argc) {
            kernel_path = argv[++i];
        } else if (arg == "--initrd" && i + 1 < argc) {
            initrd_path = argv[++i];
        } else if (arg == "--share" && i + 1 < argc) {
            share_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: ./aarchgate_daemon [OPTIONS]\n"
                      << "Options:\n"
                      << "  --kernel <path>    Path to Linux kernel Image (or 'mock')\n"
                      << "  --initrd <path>    Path to initrd ramdisk\n"
                      << "  --share <path>     Workspace directory to mount (defaults to current dir)\n"
                      << "  --help, -h         Show this help message\n";
            return 0;
        }
    }

    // 1. Initialize the JIT Policy Engine
    PolicyEngine policy_engine;

    // 2. Freeze JIT Page Regions immediately after AST compilation
    // to prevent ROP/JOP runtime tampering of compiled rules
#if defined(__arm64__) && defined(__APPLE__)
    std::cout << "[AarchGate Daemon] Securing control plane: JIT execution regions frozen." << std::endl;
    pthread_jit_write_protect_np(true); // write-protect (make executable-only)
#endif

#ifdef APEX_HAS_ICEORYX
    // Initialize Iceoryx Runtime
    iox::runtime::PoshRuntime::initRuntime("AARCHGATE_DAEMON");
    iox::popo::Publisher<MonitorEvent> monitor_publisher({
        iox::capro::IdString_t{iox::TruncateToCapacity, "AarchGate"},
        iox::capro::IdString_t{iox::TruncateToCapacity, "Sandbox"},
        iox::capro::IdString_t{iox::TruncateToCapacity, "Telemetry"}
    });

    auto monitor_cb = [&](const MonitorEvent& mon_ev) {
        monitor_publisher.loan()
            .and_then([&](auto& sample) {
                *sample = mon_ev;
                sample.publish();
            })
            .or_else([&](auto& error) {
                (void)error;
            });
    };
#else
    auto monitor_cb = [](const MonitorEvent&) {};
#endif

    // 3. Initialize VM Controller
    VMController vm_controller(kernel_path, initrd_path, share_path);

    // 4. Initialize Batch Collector with Iceoryx callback
    SyscallBatchCollector batch_collector(policy_engine, vm_controller, monitor_cb);

    // Register event callback for the VSOCK stream
    vm_controller.set_event_callback([&batch_collector](const SyscallEvent& event) {
        batch_collector.add_event(event);
    });

    // 5. Start VM Controller
    if (!vm_controller.start()) {
        std::cerr << "[AarchGate Daemon] ERROR: Failed to start VM Controller." << std::endl;
        return 1;
    }

    // Broadcast VM started state change
    {
        MonitorEvent boot_ev{};
        boot_ev.timestamp_ns = 0;
        boot_ev.pid = 0;
        boot_ev.ppid = 0;
        boot_ev.event_type = EVENT_VM_STATE_CHANGE;
        std::strcpy(boot_ev.comm, "system");
        std::strcpy(boot_ev.arg_str, "Virtual Machine Started");
        boot_ev.vm_running = 1;
        monitor_cb(boot_ev);
    }

    // 6. Start ESF Interceptor (runs failsafe in background)
    ESFManager esf_manager;
    esf_manager.start();

    std::cout << "[AarchGate Daemon] Sandbox daemon is fully initialized and guarding npm workspace." << std::endl;
    
    // Wait until shutdown signal
#if defined(__APPLE__)
    CFRunLoopRun();
#else
    std::unique_lock<std::mutex> lock(g_cv_mutex);
    g_shutdown_cv.wait(lock, []() {
        return g_shutdown.load();
    });
#endif

    // 7. Cleanup
    std::cout << "[AarchGate Daemon] Shutting down sandbox services..." << std::endl;
    
    // Broadcast VM stopped state change
    {
        MonitorEvent stop_ev{};
        stop_ev.timestamp_ns = 0;
        stop_ev.pid = 0;
        stop_ev.ppid = 0;
        stop_ev.event_type = EVENT_VM_STATE_CHANGE;
        std::strcpy(stop_ev.comm, "system");
        std::strcpy(stop_ev.arg_str, "Virtual Machine Terminated (Graceful Shutdown)");
        stop_ev.vm_running = 0;
        monitor_cb(stop_ev);
    }

    esf_manager.stop();
    vm_controller.stop();

    std::cout << "[AarchGate Daemon] Daemon exit." << std::endl;
    return 0;
}
