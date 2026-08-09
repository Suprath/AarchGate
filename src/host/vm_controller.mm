// (c) 2026 Suprath PS. All rights reserved.
// AarchGate: macOS Virtualization Bridge

#include "src/host/vm_controller.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unistd.h>
#include <sys/socket.h>

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

// Delegate class for handling Virtio socket connections on macOS
@interface VZVsockDelegate : NSObject <VZVirtioSocketListenerDelegate>
@property (nonatomic, copy) void (^connectionHandler)(int fd);
@end

@implementation VZVsockDelegate

- (BOOL)listener:(VZVirtioSocketListener *)listener shouldAcceptNewConnection:(VZVirtioSocketConnection *)connection fromSocketDevice:(VZVirtioSocketDevice *)socketDevice {
    (void)listener;
    (void)socketDevice;
    if (self.connectionHandler) {
        // Duplicate file descriptor to transfer ownership to the C++ reader thread
        int fd = dup(connection.fileDescriptor);
        self.connectionHandler(fd);
    }
    return YES;
}

@end

namespace aarchgate {

class VMControllerImpl {
public:
    std::string kernel_path;
    std::string initrd_path;
    std::string share_path;
    std::atomic<bool> running{false};
    std::function<void(const SyscallEvent&)> event_cb;

    // Apple Virtualization objects (nil in mock mode)
    VZVirtualMachine* vm = nil;
    VZVirtioSocketListener* vsock_listener = nil;
    VZVsockDelegate* vsock_delegate = nil;

    // Reader thread variables
    std::thread reader_thread;
    std::atomic<int> vsock_fd{-1};
    std::atomic<bool> stop_reader{false};

    // Thread simulating mock guest activity
    std::thread mock_thread;

    VMControllerImpl(const std::string& k, const std::string& i, const std::string& s)
        : kernel_path(k), initrd_path(i), share_path(s) {}

    ~VMControllerImpl() {
        stop();
    }

    bool start() {
        if (running) return true;

        // --- Mock Mode Check ---
        if (kernel_path == "mock") {
            running = true;
            std::cout << "[AarchGate VMController] Starting in MOCK mode..." << std::endl;
            
            // Spawn a mock telemetry stream generator thread
            stop_reader = false;
            mock_thread = std::thread([this]() {
                uint64_t mock_pid = 2000;
                while (!stop_reader) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (!event_cb) continue;

                    // Periodic benign exec event
                    SyscallEvent ev{};
                    ev.timestamp_ns = 123456789;
                    ev.pid = ++mock_pid;
                    ev.ppid = 1000; // spawned by mock npm
                    ev.event_type = EVENT_EXEC;
                    std::strcpy(ev.comm, "node");
                    std::strcpy(ev.arg_str, "benign_script.js");
                    event_cb(ev);

                    // Safe file read
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    SyscallEvent ev_open{};
                    ev_open.pid = mock_pid;
                    ev_open.ppid = 1000;
                    ev_open.event_type = EVENT_OPEN;
                    std::strcpy(ev_open.comm, "node");
                    std::strcpy(ev_open.arg_str, "/workspace/node_modules/benign/index.js");
                    event_cb(ev_open);
                }
            });
            return true;
        }

        @autoreleasepool {
            std::cout << "[AarchGate VMController] Initializing macOS Virtualization Bridge..." << std::endl;
            NSError* error = nil;
            VZVirtualMachineConfiguration* config = [[VZVirtualMachineConfiguration alloc] init];

            // 1. Configure Linux BootLoader
            VZLinuxBootLoader* bootloader = [[VZLinuxBootLoader alloc] initWithKernelURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:kernel_path.c_str()]]];
            if (!initrd_path.empty()) {
                bootloader.initialRamdiskURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:initrd_path.c_str()]];
            }
            // console=hvc0 connects guest console to the virtio channel. rdinit runs our agent.
            bootloader.commandLine = @"console=hvc0 root=/dev/ram rdinit=/sbin/init quiet";
            config.bootLoader = bootloader;

            // 2. Configure Guest Hardware
            config.CPUCount = 2; // Allocate 2 P-cores
            config.memorySize = 2LL * 1024 * 1024 * 1024; // 2 GB

            // 3. Configure Virtio-fs directory sharing
            if (!share_path.empty()) {
                NSURL* shareURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:share_path.c_str()]];
                VZSharedDirectory* sharedDir = [[VZSharedDirectory alloc] initWithURL:shareURL readOnly:NO];
                VZSingleDirectoryShare* directoryShare = [[VZSingleDirectoryShare alloc] initWithDirectory:sharedDir];
                
                VZVirtioFileSystemDeviceConfiguration* fsConfig = [[VZVirtioFileSystemDeviceConfiguration alloc] initWithTag:@"aarchgate_share"];
                fsConfig.share = directoryShare;
                config.directorySharingDevices = @[fsConfig];
            }

            // 4. Configure Virtio entropy & socket (VSOCK) devices
            VZVirtioEntropyDeviceConfiguration* entropyConfig = [[VZVirtioEntropyDeviceConfiguration alloc] init];
            config.entropyDevices = @[entropyConfig];

            VZVirtioSocketDeviceConfiguration* vsockConfig = [[VZVirtioSocketDeviceConfiguration alloc] init];
            config.socketDevices = @[vsockConfig];

            // 5. Headless console routing
            VZVirtioConsoleDeviceSerialPortConfiguration* serialConfig = [[VZVirtioConsoleDeviceSerialPortConfiguration alloc] init];
            VZFileHandleSerialPortAttachment* attachment = [[VZFileHandleSerialPortAttachment alloc] 
                initWithFileHandleForReading:[NSFileHandle fileHandleWithNullDevice] 
                fileHandleForWriting:[NSFileHandle fileHandleWithNullDevice]];
            serialConfig.attachment = attachment;
            config.serialPorts = @[serialConfig];

            // Validate configuration
            if (![config validateWithError:&error]) {
                std::cerr << "[AarchGate VMController] Validation failed: " 
                          << [error.localizedDescription UTF8String] << std::endl;
                return false;
            }

            // Instantiate VM
            vm = [[VZVirtualMachine alloc] initWithConfiguration:config];

            // 6. Set up VSOCK Listener on the Host
            vsock_listener = [[VZVirtioSocketListener alloc] init];
            vsock_delegate = [[VZVsockDelegate alloc] init];
            
            // Set up connection block
            __block VMControllerImpl* weakSelf = this;
            vsock_delegate.connectionHandler = ^(int fd) {
                weakSelf->start_reader(fd);
            };
            vsock_listener.delegate = vsock_delegate;

            // Configure socket listener on the VM's Virtio socket device
            if (vm.socketDevices.count > 0) {
                VZVirtioSocketDevice *socketDevice = (VZVirtioSocketDevice *)vm.socketDevices.firstObject;
                [socketDevice setSocketListener:vsock_listener forPort:VSOCK_TRACE_PORT];
            }

            // Start VM asynchronously
            [vm startWithCompletionHandler:^(NSError * _Nullable startError) {
                if (startError) {
                    std::cerr << "[AarchGate VMController] VM start failed: " 
                              << [startError.localizedDescription UTF8String] << std::endl;
                    weakSelf->running = false;
                } else {
                    std::cout << "[AarchGate VMController] VM started successfully." << std::endl;
                    weakSelf->running = true;
                }
            }];
            
            // Allow some time for boot setup
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return true;
        }
    }

    void stop() {
        if (!running) return;

        std::cout << "[AarchGate VMController] Stopping VM..." << std::endl;
        stop_reader = true;

        if (vsock_fd != -1) {
            close(vsock_fd);
            vsock_fd = -1;
        }

        if (reader_thread.joinable()) {
            reader_thread.join();
        }

        if (mock_thread.joinable()) {
            mock_thread.join();
        }

        if (kernel_path == "mock") {
            running = false;
            return;
        }

        @autoreleasepool {
            if (vm) {
                // Use std::shared_ptr to safely share synchronization state across languages
                auto stop_done = std::make_shared<bool>(false);
                auto stop_mutex = std::make_shared<std::mutex>();
                auto stop_cv = std::make_shared<std::condition_variable>();

                [vm stopWithCompletionHandler:^(NSError * _Nullable stopError) {
                    if (stopError) {
                        std::cerr << "[AarchGate VMController] Stop error: " 
                                  << [stopError.localizedDescription UTF8String] << std::endl;
                    } else {
                        std::cout << "[AarchGate VMController] VM stopped." << std::endl;
                    }
                    {
                        std::lock_guard<std::mutex> lock(*stop_mutex);
                        *stop_done = true;
                    }
                    stop_cv->notify_all();
                }];

                // Block caller thread until async stop completes
                std::unique_lock<std::mutex> lock(*stop_mutex);
                stop_cv->wait(lock, [stop_done]() { return *stop_done; });
                vm = nil;
            }
            vsock_listener = nil;
            vsock_delegate = nil;
        }
        running = false;
    }

    void start_reader(int fd) {
        stop_reader = false;
        vsock_fd = fd;
        reader_thread = std::thread([this]() {
            std::vector<uint8_t> buffer;
            buffer.reserve(sizeof(SyscallEvent) * 4);
            std::cout << "[AarchGate VMController] VSOCK connection established from guest." << std::endl;

            while (!stop_reader) {
                uint8_t temp[4096];
                ssize_t bytes_read = read(vsock_fd, temp, sizeof(temp));
                if (bytes_read <= 0) {
                    std::cout << "[AarchGate VMController] VSOCK connection closed." << std::endl;
                    break;
                }

                buffer.insert(buffer.end(), temp, temp + bytes_read);

                while (buffer.size() >= sizeof(SyscallEvent)) {
                    SyscallEvent event;
                    std::memcpy(&event, buffer.data(), sizeof(SyscallEvent));
                    buffer.erase(buffer.begin(), buffer.begin() + sizeof(SyscallEvent));

                    if (event_cb) {
                        event_cb(event);
                    }
                }
            }
        });
    }
};

VMController::VMController(const std::string& kernel_path, const std::string& initrd_path, const std::string& share_path)
    : impl_(std::make_unique<VMControllerImpl>(kernel_path, initrd_path, share_path)) {}

VMController::~VMController() = default;

bool VMController::start() {
    return impl_->start();
}

void VMController::stop() {
    impl_->stop();
}

bool VMController::is_running() const {
    return impl_->running;
}

void VMController::set_event_callback(std::function<void(const SyscallEvent&)> cb) {
    impl_->event_cb = std::move(cb);
}

} // namespace aarchgate
