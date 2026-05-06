#ifdef __APPLE__
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "apex/gpu/metal_device.hpp"
#include <iostream>
#include <cstdlib>
#include <cstring>

namespace apex::gpu {

MetalDevice& MetalDevice::instance() noexcept {
    static MetalDevice singleton;
    return singleton;
}

MetalDevice::MetalDevice() noexcept {
    initialize();
}

MetalDevice::~MetalDevice() noexcept {
    @autoreleasepool {
        for (auto& pair : pipeline_cache_) {
            id<MTLComputePipelineState> ps = (__bridge_transfer id<MTLComputePipelineState>)pair.second;
            ps = nil;
        }
        pipeline_cache_.clear();

        if (command_queue_) {
            id<MTLCommandQueue> queue = (__bridge_transfer id<MTLCommandQueue>)command_queue_;
            queue = nil;
        }
        if (device_) {
            id<MTLDevice> dev = (__bridge_transfer id<MTLDevice>)device_;
            dev = nil;
        }
    }
}

bool MetalDevice::initialize() noexcept {
    if (initialized_) return true;

    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            std::cerr << "[AARCHGATE ERROR] Failed to create system default Metal Device! GPU Backend Disabled." << std::endl;
            return false;
        }

        id<MTLCommandQueue> queue = [dev newCommandQueue];
        if (!queue) {
            std::cerr << "[AARCHGATE ERROR] Failed to create Metal Command Queue! GPU Backend Disabled." << std::endl;
            return false;
        }

        device_ = (__bridge_retained void*)dev;
        command_queue_ = (__bridge_retained void*)queue;
        initialized_ = true;
        
        std::cout << "[AARCHGATE INFO] Metal Device Initialized Successfully on: " 
                  << [dev.name UTF8String] << " (Unified Memory: " 
                  << (dev.hasUnifiedMemory ? "YES" : "NO") << ")" << std::endl;
    }
    return true;
}

void* MetalDevice::create_buffer_from_shm(void* ptr, size_t size) noexcept {
    if (!initialized_ || !ptr || size == 0) return nullptr;

    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)device_;
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

        // macOS memory pages require 4096-byte boundaries for zero-copy GPU mapping
        if ((addr & 4095) == 0) {
            id<MTLBuffer> buf = [dev newBufferWithBytesNoCopy:ptr
                                                       length:size
                                                      options:MTLResourceStorageModeShared
                                                  deallocator:nil];
            if (!buf) {
                std::cerr << "[AARCHGATE ERROR] newBufferWithBytesNoCopy failed for aligned pointer!" << std::endl;
                return nullptr;
            }
            return (__bridge_retained void*)buf;
        } else {
            std::cerr << "[AARCHGATE WARNING] Shared memory pointer (" << ptr 
                      << ") is NOT 4096-byte aligned. Executing aligned fallback copy!" << std::endl;

            void* aligned_ptr = nullptr;
            if (posix_memalign(&aligned_ptr, 4096, size) != 0) {
                std::cerr << "[AARCHGATE ERROR] posix_memalign allocation failed during fallback mapping!" << std::endl;
                return nullptr;
            }

            std::memcpy(aligned_ptr, ptr, size);

            id<MTLBuffer> buf = [dev newBufferWithBytesNoCopy:aligned_ptr
                                                       length:size
                                                      options:MTLResourceStorageModeShared
                                                  deallocator:^(void* pointer, NSUInteger len) {
                                                      std::free(pointer);
                                                  }];
            if (!buf) {
                std::cerr << "[AARCHGATE ERROR] newBufferWithBytesNoCopy failed for fallback aligned pointer!" << std::endl;
                std::free(aligned_ptr);
                return nullptr;
            }
            return (__bridge_retained void*)buf;
        }
    }
}

void MetalDevice::free_buffer(void* buffer_ptr) noexcept {
    if (!buffer_ptr) return;
    @autoreleasepool {
        id<MTLBuffer> buf = (__bridge_transfer id<MTLBuffer>)buffer_ptr;
        buf = nil;
    }
}

uint64_t MetalDevice::dispatch(std::string_view schema_name,
                              const void* bit_planes,
                              size_t num_blocks,
                              size_t num_fields,
                              const std::string& msl_source,
                              const std::vector<int64_t>& delta_weights,
                              uint64_t base_sum) noexcept {
    if (!initialized_ || !bit_planes || num_blocks == 0 || num_fields == 0) return 0;

    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)device_;
        id<MTLComputePipelineState> pipeline_state = nil;

        std::string key(schema_name);
        auto it = pipeline_cache_.find(key);
        if (it != pipeline_cache_.end()) {
            pipeline_state = (__bridge id<MTLComputePipelineState>)it->second;
        } else {
            // Compile MSL at runtime and cache the Compute Pipeline State
            NSString* source = [NSString stringWithUTF8String:msl_source.c_str()];
            NSError* error = nil;
            MTLCompileOptions* options = [MTLCompileOptions new];
            options.fastMathEnabled = YES;
            options.languageVersion = MTLLanguageVersion3_0;

            id<MTLLibrary> library = [dev newLibraryWithSource:source options:options error:&error];
            if (!library) {
                std::cerr << "[AARCHGATE ERROR] MSL compilation failed: " 
                          << [[error localizedDescription] UTF8String] << std::endl;
                if ([[error localizedFailureReason] length] > 0) {
                    std::cerr << "Reason: " << [[error localizedFailureReason] UTF8String] << std::endl;
                }
                return 0;
            }

            id<MTLFunction> function = [library newFunctionWithName:@"bit_sliced_evaluate"];
            if (!function) {
                std::cerr << "[AARCHGATE ERROR] Function bit_sliced_evaluate not found in compiled library!" << std::endl;
                return 0;
            }

            pipeline_state = [dev newComputePipelineStateWithFunction:function error:&error];
            if (!pipeline_state) {
                std::cerr << "[AARCHGATE ERROR] Compute pipeline state creation failed: " 
                          << [[error localizedDescription] UTF8String] << std::endl;
                return 0;
            }

            pipeline_cache_[key] = (__bridge_retained void*)pipeline_state;
        }

        // 1. Map columns zero-copy
        size_t raw_columns_size = num_blocks * num_fields * 64 * sizeof(uint64_t);
        id<MTLBuffer> columns_buf = (__bridge id<MTLBuffer>)create_buffer_from_shm(const_cast<void*>(bit_planes), raw_columns_size);
        if (!columns_buf) {
            std::cerr << "[AARCHGATE ERROR] Failed to map input columns to GPU buffer!" << std::endl;
            return 0;
        }

        // 2. Allocate output device buffer for block-buffered parallel summation (Stage 2)
        id<MTLBuffer> output_sum_buf = [dev newBufferWithLength:num_blocks * sizeof(uint64_t) options:MTLResourceStorageModeShared];
        uint64_t* sum_ptr = (uint64_t*)[output_sum_buf contents];
        std::memset(sum_ptr, 0, num_blocks * sizeof(uint64_t));

        // 3. Dispatch GPU Compute Shaders
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue_;
        id<MTLCommandBuffer> cmd_buffer = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [cmd_buffer computeCommandEncoder];

        [encoder setComputePipelineState:pipeline_state];
        [encoder setBuffer:columns_buf offset:0 atIndex:0];
        [encoder setBuffer:output_sum_buf offset:0 atIndex:1];

        // Threadgroup is configured to match 64 threads (one block of 64 rows evaluated in register bitplanes)
        MTLSize threadgroup_size = MTLSizeMake(64, 1, 1);
        MTLSize grid_size = MTLSizeMake(num_blocks * 64, 1, 1);

        [encoder dispatchThreads:grid_size threadsPerThreadgroup:threadgroup_size];
        [encoder endEncoding];

        [cmd_buffer commit];
        [cmd_buffer waitUntilCompleted];

        uint64_t final_sum = 0;
        for (size_t i = 0; i < num_blocks; ++i) {
            final_sum += sum_ptr[i];
        }

        // Cleanup temporary zero-copy buffer references
        free_buffer((__bridge void*)columns_buf);
        output_sum_buf = nil;

        return final_sum;
    }
}

} // namespace apex::gpu
#endif
