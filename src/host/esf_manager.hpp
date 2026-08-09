// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: macOS Endpoint Security Framework Interceptor Header

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>

namespace aarchgate {

class ESFManager {
public:
    ESFManager();
    ~ESFManager();

    // Starts the Endpoint Security subscription client
    bool start();

    // Stops the client and releases subscriptions
    void stop();

    // Returns true if ESF is actively intercepting processes
    bool is_active() const;

    // Registers a callback invoked when a host-side package manager execution is intercepted
    void set_intercept_callback(std::function<void(const std::string& path, const std::vector<std::string>& args)> cb);

private:
    std::atomic<bool> active_{false};
    std::function<void(const std::string&, const std::vector<std::string>&)> intercept_cb_;

    // Opaque pointer to the ESF client (es_client_t)
    void* es_client_ = nullptr;

    bool init_esf_client();
};

} // namespace aarchgate
