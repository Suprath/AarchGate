#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef APEX_EXPORT
        #define APEX_API __declspec(dllexport)
    #else
        #define APEX_API __declspec(dllimport)
    #endif
#else
    #if __GNUC__ >= 4
        #define APEX_API __attribute__((visibility("default")))
    #else
        #define APEX_API
    #endif
#endif

namespace apex {

enum class ExecutionMode : int {
    BIT_SLICED = 0,
    SCALAR = 1,
    GPU_THROUGHPUT = 2
};

} // namespace apex
