#pragma once

#ifdef __APPLE__
#include "apex/common.hpp"
#include <string_view>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <string>

namespace apex::gpu {

class APEX_API MetalDevice {
public:
    // Access the global thread-safe singleton
    static MetalDevice& instance() noexcept;

    // Initialize the device and command queues
    bool initialize() noexcept;
    bool is_initialized() const noexcept { return initialized_; }

    // Retrieve raw MTLDevice pointer
    void* get_device() const noexcept { return device_; }

    // Zero-Copy Bridge: Maps host page-aligned pointers directly to GPU MTLBuffers
    void* create_buffer_from_shm(void* ptr, size_t size) noexcept;
    
    // Deallocate / release a mapped MTLBuffer pointer
    void free_buffer(void* buffer_ptr) noexcept;

    // Dispatches a JIT expression logic pipeline across the GPU
    uint64_t dispatch(std::string_view schema_name,
                      const void* bit_planes,
                      size_t num_blocks,
                      size_t num_fields,
                      const std::string& msl_source,
                      const std::vector<int64_t>& delta_weights,
                      uint64_t base_sum) noexcept;

private:
    MetalDevice() noexcept;
    ~MetalDevice() noexcept;

    // Block copies and assignments to protect singleton structure
    MetalDevice(const MetalDevice&) = delete;
    MetalDevice& operator=(const MetalDevice&) = delete;

    void* device_ = nullptr;        // id<MTLDevice>
    void* command_queue_ = nullptr;  // id<MTLCommandQueue>
    std::unordered_map<std::string, void*> pipeline_cache_; // schema_name -> id<MTLComputePipelineState>
    bool initialized_ = false;
};

} // namespace apex::gpu
#endif
