// External Linkage Barrier - Compiled with -fno-lto
// This forces actual execution across object boundaries with volatile semantics
#include "apex/AarchGate.hpp"

// Use volatile qualifier to prevent inlining and optimization
extern "C" {
    volatile uint64_t external_execute(apex::ApexEngine* engine, const void* data, size_t count) __attribute__((noinline)) {
        // Use volatile to prevent the compiler from optimizing away the call
        volatile uint64_t result = engine->execute(data, count);
        // Hardware barrier to ensure result is actually computed
        __asm__ __volatile__("" : "+r" (result) : : "memory");
        return result;
    }
}
