// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: Sandbox Integration & Verification Test

#include "host/policy_engine.hpp"
#include "host/vm_controller.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>
#include <thread>
#include <chrono>

using namespace aarchgate;

int main() {
    std::cout << "=== AarchGate Sandbox Integration & Verification Test ===" << std::endl;

    // 1. Initialize Policy Engine
    PolicyEngine policy_engine;

    // 2. Initialize VM Controller in Mock Mode
    VMController vm("mock", "", ".");

    // Set callback to verify event ingestion
    vm.set_event_callback([](const SyscallEvent& event) {
        (void)event;
    });

    // Start VM
    assert(vm.start() == true);
    assert(vm.is_running() == true);
    std::cout << "[Integration Test] Mock VM started." << std::endl;

    // 3. Prepare benign batch (64 events)
    std::vector<SyscallTraceRecord> batch(64);
    for (int i = 0; i < 64; ++i) {
        SyscallEvent ev{};
        ev.pid = 2000 + i;
        ev.ppid = 1000;
        ev.event_type = EVENT_OPEN;
        std::strcpy(ev.comm, "node");
        std::strcpy(ev.arg_str, "/workspace/node_modules/lodash/index.js");
        batch[i] = policy_engine.process_event(ev);
    }

    // Assert that the benign batch triggers zero violations
    uint64_t violations = policy_engine.evaluate_batch(batch.data(), 64);
    assert(violations == 0);
    std::cout << "[Integration Test] Safe batch evaluation passed (0 violations)." << std::endl;

    // 4. Register a simulated npm preinstall script execution
    SyscallEvent ev_npm_exec{};
    ev_npm_exec.pid = 100;
    ev_npm_exec.ppid = 99; // parent shell
    ev_npm_exec.event_type = EVENT_EXEC;
    std::strcpy(ev_npm_exec.comm, "npm");
    std::strcpy(ev_npm_exec.arg_str, "run preinstall");
    policy_engine.process_event(ev_npm_exec); // Registers PID 100 as preinstall hook

    // Simulate child process spawned under preinstall hook
    SyscallEvent ev_node_exec{};
    ev_node_exec.pid = 101;
    ev_node_exec.ppid = 100;
    ev_node_exec.event_type = EVENT_EXEC;
    std::strcpy(ev_node_exec.comm, "node");
    std::strcpy(ev_node_exec.arg_str, "install.js");
    policy_engine.process_event(ev_node_exec); // Registers PID 101 as child of preinstall hook

    // Simulate malicious event: PID 101 opening SSH keys
    SyscallEvent mal_ev{};
    mal_ev.pid = 101;
    mal_ev.ppid = 100;
    mal_ev.event_type = EVENT_OPEN;
    std::strcpy(mal_ev.comm, "node");
    std::strcpy(mal_ev.arg_str, "/Users/suprathps/.ssh/id_rsa");

    // Inject malicious event at index 42 of the batch
    batch[42] = policy_engine.process_event(mal_ev);

    // Evaluate the batch using the JIT engine
    violations = policy_engine.evaluate_batch(batch.data(), 64);
    assert(violations == 1);
    std::cout << "[Integration Test] Malicious batch evaluation passed (1 violation detected)." << std::endl;

    // 5. Trigger VM Kill Switch
    std::cout << "[Integration Test] Triggering VM kill switch..." << std::endl;
    vm.stop();
    assert(vm.is_running() == false);
    std::cout << "[Integration Test] Mock VM stopped." << std::endl;

    std::cout << "✓ Sandbox integration test passed successfully!" << std::endl;
    return 0;
}
