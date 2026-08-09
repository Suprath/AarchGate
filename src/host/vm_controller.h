// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: macOS Virtualization Bridge Header

#pragma once

#include <string>
#include <memory>
#include <functional>
#include "common/vsock_protocol.h"

namespace aarchgate {

class VMControllerImpl;

class VMController {
public:
    VMController(const std::string& kernel_path, 
                 const std::string& initrd_path, 
                 const std::string& share_path);
    ~VMController();

    // Boots the Linux guest VM inside Virtualization.framework
    bool start();

    // Shuts down the VM immediately
    void stop();

    // Check VM execution status
    bool is_running() const;

    // Registers a callback to handle syscall traces received over VSOCK
    void set_event_callback(std::function<void(const SyscallEvent&)> cb);

private:
    std::unique_ptr<VMControllerImpl> impl_;
};

} // namespace aarchgate
