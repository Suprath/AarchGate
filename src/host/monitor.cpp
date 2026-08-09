// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: Real-Time Telemetry Monitor (TUI)

#include <iostream>
#include <vector>
#include <map>
#include <queue>
#include <string>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "common/monitor_protocol.h"
#include "iceoryx_posh/runtime/posh_runtime.hpp"
#include "iceoryx_posh/popo/subscriber.hpp"

using namespace aarchgate;

// ANSI Terminal Colors
#define RESET       "\033[0m"
#define BOLD        "\033[1m"
#define RED         "\033[31m"
#define GREEN       "\033[32m"
#define YELLOW      "\033[33m"
#define BLUE        "\033[34m"
#define MAGENTA     "\033[35m"
#define CYAN        "\033[36m"
#define WHITE       "\033[37m"
#define BG_RED      "\033[41m"
#define CLEAR_SCR   "\033[2J\033[H"
#define HIDE_CURSOR "\033[?25l"
#define SHOW_CURSOR "\033[?25h"

std::atomic<bool> g_stop{false};

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop = true;
    }
}

// Process representation
struct ProcessNode {
    uint64_t pid;
    uint64_t ppid;
    std::string comm;
    std::string arg;
    bool is_preinstall;
    std::vector<uint64_t> children;
};

// Global telemetry model
struct TelemetryModel {
    bool vm_running = false;
    std::string vm_status_desc = "Offline";
    
    // Global stats
    uint64_t total_execs = 0;
    uint64_t total_opens = 0;
    uint64_t total_connects = 0;
    uint64_t total_mte_traps = 0;
    uint64_t total_violations = 0;

    // Process tree mapping
    std::map<uint64_t, ProcessNode> processes;
    std::vector<uint64_t> root_pids;

    // Scrolling logs
    std::vector<MonitorEvent> logs;

    void add_event(const MonitorEvent& ev) {
        if (ev.event_type == EVENT_VM_STATE_CHANGE) {
            vm_running = (ev.vm_running == 1);
            vm_status_desc = ev.arg_str;
            return;
        }

        // Add to logs
        logs.push_back(ev);
        if (logs.size() > 12) {
            logs.erase(logs.begin());
        }

        // Stats accumulation
        if (ev.event_type == EVENT_EXEC) total_execs++;
        else if (ev.event_type == EVENT_OPEN) total_opens++;
        else if (ev.event_type == EVENT_CONNECT) total_connects++;
        else if (ev.event_type == EVENT_MTE_TRAP) total_mte_traps++;

        if (ev.is_sensitive && ev.is_preinstall && ev.event_type == EVENT_OPEN) total_violations++;
        if (ev.is_unauthorized && ev.is_preinstall && ev.event_type == EVENT_CONNECT) total_violations++;

        // Process tree updates
        if (ev.event_type == EVENT_EXEC) {
            // Check if process already exists
            if (processes.find(ev.pid) == processes.end()) {
                ProcessNode node{};
                node.pid = ev.pid;
                node.ppid = ev.ppid;
                node.comm = ev.comm;
                node.arg = ev.arg_str;
                node.is_preinstall = (ev.is_preinstall == 1);

                processes[ev.pid] = node;

                // Link to parent or treat as root
                if (ev.ppid != 0 && processes.find(ev.ppid) != processes.end()) {
                    processes[ev.ppid].children.push_back(ev.pid);
                } else {
                    root_pids.push_back(ev.pid);
                }
            }
        }
    }
};

// Recursive helper to render the process tree in ASCII
void draw_process_node(const TelemetryModel& model, uint64_t pid, const std::string& prefix, bool is_last) {
    auto it = model.processes.find(pid);
    if (it == model.processes.end()) return;
    
    const auto& node = it->second;
    
    std::cout << prefix << (is_last ? "└── " : "├── ");
    
    // Highlight preinstall processes in yellow
    if (node.is_preinstall) {
        std::cout << YELLOW << BOLD << node.comm << RESET << " (PID: " << node.pid << ")";
    } else {
        std::cout << GREEN << node.comm << RESET << " (PID: " << node.pid << ")";
    }
    
    if (!node.arg.empty()) {
        std::cout << " " << WHITE << "[" << node.arg << "]" << RESET;
    }
    std::cout << "\n";

    std::string new_prefix = prefix + (is_last ? "    " : "│   ");
    for (size_t i = 0; i < node.children.size(); ++i) {
        draw_process_node(model, node.children[i], new_prefix, i == node.children.size() - 1);
    }
}

void render_dashboard(const TelemetryModel& model) {
    std::cout << CLEAR_SCR;
    std::cout << BOLD << CYAN << "================================================================================\n";
    std::cout << "                 AarchGate Zero-Trust Sandbox Monitor v1.0                      \n";
    std::cout << "================================================================================\n" << RESET;

    // 1. VM Status Panel
    std::cout << BOLD << "VM Lifecycle: " << RESET;
    if (model.vm_running) {
        std::cout << GREEN << BOLD << "[RUNNING] " << RESET << "(" << model.vm_status_desc << ")\n";
    } else {
        bool killed_by_policy = (model.total_violations > 0) ||
                                (model.vm_status_desc.find("Kill Switch") != std::string::npos);
        if (killed_by_policy) {
            std::cout << RED << BOLD << "[KILLED]  " << RESET << "(" << model.vm_status_desc << ")\n";
        } else if (model.logs.empty() && model.total_execs == 0) {
            std::cout << YELLOW << BOLD << "[WAITING] " << RESET << "(Waiting for aarchgate_daemon to connect...)\n";
        } else {
            std::cout << WHITE << BOLD << "[OFFLINE] " << RESET << "(" << model.vm_status_desc << ")\n";
        }
    }

    // 2. Telemetry Statistics
    std::cout << BOLD << "\n[Telemetry Statistics]\n" << RESET;
    std::cout << "  Spawns (EXEC): " << BOLD << model.total_execs << RESET
              << "   |   File Opens: " << BOLD << model.total_opens << RESET
              << "   |   Connections: " << BOLD << model.total_connects << RESET << "\n";
    std::cout << "  MTE Traps:     " << BOLD << (model.total_mte_traps > 0 ? RED : GREEN) << model.total_mte_traps << RESET
              << "   |   Violations: " << BOLD << (model.total_violations > 0 ? RED : GREEN) << model.total_violations << RESET << "\n";
    std::cout << "--------------------------------------------------------------------------------\n";

    // 3. Process Tree Panel
    std::cout << BOLD << "\n[Sandbox Process Tree Hierarchy]\n" << RESET;
    if (model.root_pids.empty()) {
        std::cout << "  No processes spawned in sandbox yet.\n";
    } else {
        for (size_t i = 0; i < model.root_pids.size(); ++i) {
            draw_process_node(model, model.root_pids[i], "  ", i == model.root_pids.size() - 1);
        }
    }
    std::cout << "\n--------------------------------------------------------------------------------\n";

    // 4. Intercepted Syscall Log Panel
    std::cout << BOLD << "\n[Real-Time Intercepted Syscall Logs]\n" << RESET;
    if (model.logs.empty()) {
        std::cout << "  Awaiting syscall telemetry stream...\n";
    } else {
        for (const auto& log : model.logs) {
            // Trim comm to 15 chars (TASK_COMM_LEN) for clean display
            std::string comm = std::string(log.comm).substr(0, 15);

            std::cout << "  ";
            if (log.event_type == EVENT_MTE_TRAP) {
                std::cout << BG_RED << BOLD << "[MTE TRAP]   " << RESET << " " << RED << comm << RESET
                          << " violated hardware tags at " << RED << log.arg_str << RESET << "\n";
            } else if (log.is_sensitive && log.is_preinstall && log.event_type == EVENT_OPEN) {
                std::cout << RED << BOLD << "[BLOCKED OPEN]  " << RESET << " preinstall " << RED << comm << RESET
                          << " tried to read: " << RED << log.arg_str << RESET << "\n";
            } else if (log.is_unauthorized && log.is_preinstall && log.event_type == EVENT_CONNECT) {
                std::cout << RED << BOLD << "[BLOCKED CONN]  " << RESET << " preinstall " << RED << comm << RESET
                          << " exfil attempt: " << RED << log.arg_str << ":" << log.port << RESET << "\n";
            } else if (log.event_type == EVENT_EXEC) {
                std::cout << GREEN << BOLD << "[EXEC]   " << RESET
                          << " PID " << std::setw(5) << log.pid << " "
                          << BOLD << std::left << std::setw(15) << comm << RESET
                          << " -> " << log.arg_str << "\n";
            } else if (log.event_type == EVENT_OPEN) {
                std::cout << CYAN << "[OPEN]   " << RESET
                          << " PID " << std::setw(5) << log.pid << " "
                          << std::left << std::setw(15) << comm
                          << " path: " << log.arg_str << "\n";
            } else if (log.event_type == EVENT_CONNECT) {
                // Format IPv4 from uint32 if arg_str is empty
                std::string dest = std::string(log.arg_str);
                if (dest.empty() || dest == "other_connect") {
                    uint32_t ip = log.ip_address;
                    dest = std::to_string((ip >> 24) & 0xFF) + "." +
                           std::to_string((ip >> 16) & 0xFF) + "." +
                           std::to_string((ip >> 8)  & 0xFF) + "." +
                           std::to_string(ip & 0xFF);
                }
                std::cout << MAGENTA << "[CONNECT]" << RESET
                          << " PID " << std::setw(5) << log.pid << " "
                          << std::left << std::setw(15) << comm
                          << " -> " << dest << ":" << log.port << "\n";
            }
        }
    }
    std::cout << std::endl;
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Hide console cursor for full dashboard experience
    std::cout << HIDE_CURSOR << std::flush;

    std::cout << "[AarchGate Monitor] Initializing Iceoryx shared-memory runtime..." << std::endl;

    // 1. Initialize PoshRuntime
    try {
        iox::runtime::PoshRuntime::initRuntime("AARCHGATE_MONITOR");
    } catch (...) {
        std::cerr << "[AarchGate Monitor] ERROR: Failed to init POSH runtime. Is RouDi running? (iox-roudi)" << std::endl;
        std::cout << SHOW_CURSOR << std::endl;
        return 1;
    }

    // 2. Initialize Telemetry Subscriber
    iox::popo::Subscriber<MonitorEvent> subscriber({
        iox::capro::IdString_t{iox::TruncateToCapacity, "AarchGate"},
        iox::capro::IdString_t{iox::TruncateToCapacity, "Sandbox"},
        iox::capro::IdString_t{iox::TruncateToCapacity, "Telemetry"}
    });

    TelemetryModel model;

    // Clear screen and render initial "waiting" dashboard immediately
    std::cout << CLEAR_SCR << std::flush;
    render_dashboard(model);

    // 3. Polling loop - refresh every 500ms regardless of events
    auto last_render = std::chrono::steady_clock::now();
    while (!g_stop) {
        bool updated = false;

        // Process all queued IPC messages
        while (true) {
            auto result = subscriber.take();
            if (result.has_error()) {
                break; // No more samples in queue
            }

            const auto& sample = result.value();
            model.add_event(*sample);
            updated = true;
        }

        // Re-render if new events arrived OR every 500ms for live clock/status
        auto now = std::chrono::steady_clock::now();
        if (updated || std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render).count() >= 500) {
            render_dashboard(model);
            last_render = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Restore terminal cursor on exit
    std::cout << SHOW_CURSOR << "\n[AarchGate Monitor] Exiting." << std::endl;
    return 0;
}
