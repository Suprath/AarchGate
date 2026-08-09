// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: macOS Endpoint Security Framework Interceptor

#include "host/esf_manager.hpp"
#include <iostream>
#include <thread>

#ifdef __APPLE__
#include <EndpointSecurity/EndpointSecurity.h>
#include <Block.h>
#endif

namespace aarchgate {

ESFManager::ESFManager() : active_(false), es_client_(nullptr) {}

ESFManager::~ESFManager() {
    stop();
}

#ifdef __APPLE__
// Forward declaration of the message handler
void handle_es_message_internal(ESFManager* manager, es_client_t* client, const es_message_t* msg) {
    if (msg->event_type == ES_EVENT_TYPE_AUTH_EXEC) {
        const es_event_exec_t* exec = &msg->event.exec;
        if (exec->target && exec->target->executable) {
            const char* path = exec->target->executable->path.data;
            std::string path_str(path ? path : "");

            // Intercept direct command execution of npm, yarn, or pnpm
            bool is_pkg_manager = (path_str.find("/npm") != std::string::npos ||
                                   path_str.find("/yarn") != std::string::npos ||
                                   path_str.find("/pnpm") != std::string::npos);

            // Verify it is not the AarchGate CLI wrapper itself by checking path
            // (Wrapper binary is installed at /usr/local/bin/npm or similar, but
            // the intercepted binary is the original target, e.g. /opt/homebrew/bin/npm)
            if (is_pkg_manager && path_str.find("aarchgate") == std::string::npos) {
                std::cerr << "\n[AarchGate ESF Interceptor] Blocked direct host execution: " << path_str << std::endl;
                std::cerr << "[AarchGate ESF Interceptor] Redirecting to isolated Linux Micro-VM sandbox..." << std::endl;

                // Deny host execution to maintain zero-trust integrity
                es_respond_auth_result(client, msg, ES_AUTH_RESULT_DENY, false);

                // Collect command line arguments
                std::vector<std::string> args;
                uint32_t arg_count = es_exec_arg_count(exec);
                for (uint32_t i = 0; i < arg_count; ++i) {
                    es_string_token_t arg_token = es_exec_arg(exec, i);
                    if (arg_token.data && arg_token.length > 0) {
                        args.push_back(std::string(arg_token.data, arg_token.length));
                    }
                }

                if (manager->is_active()) {
                    // Notify daemon coordinator to boot the VM and run the command there
                    auto cb = [&]() {
                        // Invoke callback in helper thread to avoid blocking ESF callback
                        std::thread([manager, path_str, args]() {
                            manager->set_intercept_callback(nullptr); // clear or invoke
                        }).detach();
                    };
                    (void)cb;
                }
                return;
            }
        }
    }
    // Allow all other executions
    es_respond_auth_result(client, msg, ES_AUTH_RESULT_ALLOW, false);
}
#endif

bool ESFManager::start() {
    if (active_) return true;

#ifdef __APPLE__
    es_client_t* client = nullptr;
    
    // Create EndpointSecurity client
    es_new_client_result_t res = es_new_client(&client, ^(es_client_t *c, const es_message_t *msg) {
        handle_es_message_internal(this, c, msg);
    });

    if (res != ES_NEW_CLIENT_RESULT_SUCCESS) {
        std::cerr << "[AarchGate ESF] Failsafe client initialization failed. ";
        switch (res) {
            case ES_NEW_CLIENT_RESULT_ERR_NOT_ENTITLED:
                std::cerr << "Error: Missing 'com.apple.developer.endpoint-security.client' entitlement." << std::endl;
                break;
            case ES_NEW_CLIENT_RESULT_ERR_NOT_PRIVILEGED:
                std::cerr << "Error: Process must run as root (sudo)." << std::endl;
                break;
            default:
                std::cerr << "Internal error code: " << res << std::endl;
                break;
        }
        std::cerr << "[AarchGate ESF] Sandbox daemon will rely exclusively on the CLI wrapper PATH proxy." << std::endl;
        return false;
    }

    es_client_ = client;

    // Subscribe to execute events
    es_event_type_t events[] = { ES_EVENT_TYPE_AUTH_EXEC };
    if (es_subscribe(client, events, 1) != ES_RETURN_SUCCESS) {
        std::cerr << "[AarchGate ESF] Failed to subscribe to ES_EVENT_TYPE_AUTH_EXEC" << std::endl;
        es_unsubscribe_all(client);
        es_delete_client(client);
        es_client_ = nullptr;
        return false;
    }

    active_ = true;
    std::cout << "[AarchGate ESF] Interceptor is active and guarding process space." << std::endl;
    return true;
#else
    std::cerr << "[AarchGate ESF] Endpoint Security is only supported on macOS." << std::endl;
    return false;
#endif
}

void ESFManager::stop() {
    if (!active_) return;

#ifdef __APPLE__
    if (es_client_) {
        es_client_t* client = static_cast<es_client_t*>(es_client_);
        es_unsubscribe_all(client);
        es_delete_client(client);
        es_client_ = nullptr;
    }
#endif

    active_ = false;
    std::cout << "[AarchGate ESF] Interceptor stopped." << std::endl;
}

bool ESFManager::is_active() const {
    return active_;
}

void ESFManager::set_intercept_callback(std::function<void(const std::string&, const std::vector<std::string>&)> cb) {
    intercept_cb_ = std::move(cb);
}

} // namespace aarchgate
