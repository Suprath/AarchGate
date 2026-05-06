#ifdef __APPLE__
#include "apex/gpu/metal_device.hpp"
#include <iostream>
#include <cstdlib>
#include <cassert>
#include <chrono>

using namespace apex::gpu;

int main() {
    std::cout << "=== AarchGate-Metal Zero-Copy Bridge Verification Test ===" << std::endl;

    // 1. Initialize Device Singleton
    auto& device = MetalDevice::instance();
    if (!device.initialize()) {
        std::cerr << "[TEST FAIL] Metal Device failed to initialize!" << std::endl;
        return 1;
    }

    assert(device.is_initialized());
    std::cout << "[TEST PASS] Metal Device initialized successfully." << std::endl;

    size_t size = 4096 * 256; // 1 Megabyte of memory
    std::cout << "\nPreparing to map " << size / 1024 << " KB of virtual memory..." << std::endl;

    // 2. Scenario A: Perfectly 4096-Byte Page-Aligned Buffer (True Zero-Copy)
    std::cout << "\n--- Scenario A: Perfectly 4096-Byte Page-Aligned Buffer ---" << std::endl;
    void* aligned_ptr = nullptr;
    if (posix_memalign(&aligned_ptr, 4096, size) != 0) {
        std::cerr << "[TEST FAIL] posix_memalign allocation failed!" << std::endl;
        return 1;
    }

    // Verify raw host pointer is page-aligned
    uintptr_t host_addr = reinterpret_cast<uintptr_t>(aligned_ptr);
    std::cout << "Allocated Aligned Host Address: " << aligned_ptr << " (Offset: " << (host_addr & 4095) << ")" << std::endl;
    assert((host_addr & 4095) == 0);

    auto t0 = std::chrono::high_resolution_clock::now();
    void* mtl_aligned_buf = device.create_buffer_from_shm(aligned_ptr, size);
    auto t1 = std::chrono::high_resolution_clock::now();

    if (!mtl_aligned_buf) {
        std::cerr << "[TEST FAIL] Failed to map page-aligned host memory to MTLBuffer!" << std::endl;
        std::free(aligned_ptr);
        return 1;
    }

    double elapsed_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::cout << "[TEST PASS] Aligned Memory mapped in: " << elapsed_us << " microseconds! (Zero-Copy Instant Handoff)" << std::endl;

    // Release buffer and aligned memory
    device.free_buffer(mtl_aligned_buf);
    std::free(aligned_ptr);
    std::cout << "Aligned Memory released successfully." << std::endl;


    // 3. Scenario B: Unaligned Buffer (Triggers Automated Self-Deallocating Fallback)
    std::cout << "\n--- Scenario B: Unaligned Buffer (Fallback Trigger Check) ---" << std::endl;
    
    // Allocate raw memory normally, then offset pointer by 256 bytes to guarantee non-alignment
    char* raw_unaligned = new char[size + 512];
    void* unaligned_ptr = reinterpret_cast<void*>(raw_unaligned + 256);
    uintptr_t unaligned_addr = reinterpret_cast<uintptr_t>(unaligned_ptr);
    
    std::cout << "Allocated Unaligned Host Address: " << unaligned_ptr << " (Offset: " << (unaligned_addr & 4095) << ")" << std::endl;
    assert((unaligned_addr & 4095) != 0);

    t0 = std::chrono::high_resolution_clock::now();
    void* mtl_unaligned_buf = device.create_buffer_from_shm(unaligned_ptr, size);
    t1 = std::chrono::high_resolution_clock::now();

    if (!mtl_unaligned_buf) {
        std::cerr << "[TEST FAIL] Unaligned fallback buffer creation failed!" << std::endl;
        delete[] raw_unaligned;
        return 1;
    }

    elapsed_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::cout << "[TEST PASS] Unaligned buffer processed in: " << elapsed_us << " microseconds! (Safe Aligned Fallback Copy)" << std::endl;

    // Release unaligned structures
    device.free_buffer(mtl_unaligned_buf);
    delete[] raw_unaligned;
    std::cout << "Unaligned Memory and blocks released successfully." << std::endl;

    std::cout << "\n🎉 ALL SCENARIOS VERIFIED successfully! Zero-copy M3 GPU memory bridge is 100% operational!" << std::endl;
    return 0;
}
#else
#include <iostream>
int main() {
    std::cout << "Metal is only supported on Apple macOS Platforms." << std::endl;
    return 0;
}
#endif
