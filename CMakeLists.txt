cmake_minimum_required(VERSION 3.18)
project(AarchGate VERSION 1.0.0 LANGUAGES CXX)

# Threads - Required by iceoryx for multi-threaded IPC
find_package(Threads REQUIRED)

# Position Independent Code for shared library
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -std=c++20")

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Extreme optimization flags for production
set(APEX_COMPILE_FLAGS_STR "-O3 -march=native -ffast-math -fno-finite-math-only -Wall -Wextra")

# Release flags (production)
set(CMAKE_CXX_FLAGS_RELEASE "${APEX_COMPILE_FLAGS_STR}")

# Debug flags with sanitizers
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")

# Optimization flags list for target_compile_options (must be a semicolon-separated list)
string(REPLACE " " ";" APEX_COMPILE_FLAGS "${APEX_COMPILE_FLAGS_STR}")

# Include directories
include_directories(include)

# Source files for OBJECT library
set(APEX_SOURCES
    src/core/core.cpp
    src/core/registry.cpp
    src/memory/memory.cpp
    src/jit/jit.cpp
    src/jit/circuit_library.cpp
    src/jit/compiler.cpp
    src/compute/compute.cpp
    src/compute/bit_slicer.cpp
    src/compute/bit_slicer_fast.cpp
    src/compute/parallel_runner.cpp
    src/api/api.cpp
    src/api/engine.cpp
    src/api/aarchgate_c_api.cpp
    src/telemetry/telemetry.cpp
)

# OBJECT library: shared compilation unit for linking into both SHARED and EXECUTABLE
add_library(apex_obj OBJECT ${APEX_SOURCES})
target_compile_options(apex_obj PRIVATE ${APEX_COMPILE_FLAGS})
target_include_directories(apex_obj PUBLIC include)

# (FlatBuffers generation moved to examples/trading)

# External Dependencies: Try submodules first, then system packages

# AsmJit - JIT code generation (static library)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/asmjit/CMakeLists.txt")
    message(STATUS "Building AsmJit from submodule")
    set(ASMJIT_STATIC ON CACHE BOOL "Build AsmJit as static library")
    add_subdirectory(external/asmjit)
    set(asmjit_FOUND TRUE)
else()
    find_package(asmjit QUIET)
    if(NOT asmjit_FOUND)
        message(STATUS "AsmJit not found; JIT features will be disabled")
    endif()
endif()

# Google Highway - Portable SIMD
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/highway/CMakeLists.txt")
    message(STATUS "Building Google Highway from submodule")
    set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "Disable Highway examples")
    set(HWY_ENABLE_TESTS OFF CACHE BOOL "Disable Highway tests")
    add_subdirectory(external/highway)
    set(highway_FOUND TRUE)
else()
    find_package(highway QUIET)
    if(NOT highway_FOUND)
        message(STATUS "Google Highway not found; SIMD features will be disabled")
    endif()
endif()

# iceoryx - Zero-copy IPC (platform, hoofs, and posh modules)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/iceoryx/iceoryx_platform/CMakeLists.txt")
    message(STATUS "Building iceoryx (platform/hoofs/posh) from submodule")
    set(IOX_BUILD_TESTS OFF CACHE BOOL "Disable iceoryx tests")
    set(IOX_BUILD_EXAMPLES OFF CACHE BOOL "Disable iceoryx examples")
    
    # Optional ACL support (Defaults to ON for compatibility, but can be disabled)
    option(IOX_PLATFORM_FEATURE_ACL "Enable iceoryx ACL support" ON)
    
    set(IOX_VERSION_STRING "2.0.0" CACHE STRING "iceoryx version string")
    # Build platform first (dependency of hoofs)
    add_subdirectory(external/iceoryx/iceoryx_platform)
    # Build hoofs and posh (depend on platform)
    add_subdirectory(external/iceoryx/iceoryx_hoofs)
    add_subdirectory(external/iceoryx/iceoryx_posh)
    set(iceoryx_FOUND TRUE)
else()
    find_package(iceoryx_binding_c QUIET)
    if(NOT iceoryx_binding_c_FOUND)
        message(STATUS "iceoryx not found; IPC features will be disabled")
    else()
        set(iceoryx_FOUND TRUE)
    endif()
endif()

# FlatBuffers - Serialization (with flatc compiler)
option(APEX_BUILD_FLATBUFFERS "Build FlatBuffers support" ON)
if(APEX_BUILD_FLATBUFFERS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/flatbuffers/CMakeLists.txt")
        message(STATUS "Building FlatBuffers from submodule")
        set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "Disable FlatBuffers tests")
        set(FLATBUFFERS_BUILD_FLATLIB OFF CACHE BOOL "Disable FlatLib")
        set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "Build flatc compiler")
        add_subdirectory(external/flatbuffers)
        set(flatbuffers_FOUND TRUE)
    else()
        find_package(flatbuffers QUIET)
        if(NOT flatbuffers_FOUND)
            message(STATUS "FlatBuffers not found; serialization features will be disabled")
        endif()
    endif()
endif()

# (FlatBuffers headers generated in examples/trading)

# Link external libraries to apex_obj (shared compilation unit)
target_link_libraries(apex_obj PRIVATE Threads::Threads)

if(asmjit_FOUND)
    target_link_libraries(apex_obj PRIVATE asmjit::asmjit)
    target_compile_definitions(apex_obj PRIVATE APEX_HAS_ASMJIT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/asmjit")
        target_include_directories(apex_obj PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/asmjit")
    endif()
endif()

if(highway_FOUND)
    target_link_libraries(apex_obj PRIVATE hwy)
    target_compile_definitions(apex_obj PRIVATE APEX_HAS_HIGHWAY)
endif()

if(iceoryx_FOUND)
    target_link_libraries(apex_obj PRIVATE iceoryx_hoofs iceoryx_posh)
    target_compile_definitions(apex_obj PRIVATE APEX_HAS_ICEORYX)
endif()

if(flatbuffers_FOUND)
    target_compile_definitions(apex_obj PRIVATE APEX_HAS_FLATBUFFERS)
endif()

# SHARED library: aarchgate runtime library
add_library(aarchgate SHARED
    $<TARGET_OBJECTS:apex_obj>
)
target_compile_options(aarchgate PRIVATE ${APEX_COMPILE_FLAGS})
target_include_directories(aarchgate PUBLIC include)
set_target_properties(aarchgate PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)



# Propagate external library links to aarchgate shared library
target_link_libraries(aarchgate PRIVATE Threads::Threads)

if(asmjit_FOUND)
    target_link_libraries(aarchgate PRIVATE asmjit::asmjit)
    target_compile_definitions(aarchgate PUBLIC APEX_HAS_ASMJIT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/asmjit")
        target_include_directories(aarchgate PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/external/asmjit")
    endif()
endif()

if(highway_FOUND)
    target_link_libraries(aarchgate PRIVATE hwy)
    target_compile_definitions(aarchgate PRIVATE APEX_HAS_HIGHWAY)
endif()

if(iceoryx_FOUND)
    target_link_libraries(aarchgate PRIVATE iceoryx_hoofs iceoryx_posh)
    target_compile_definitions(aarchgate PRIVATE APEX_HAS_ICEORYX)
endif()

if(flatbuffers_FOUND)
    target_compile_definitions(aarchgate PRIVATE APEX_HAS_FLATBUFFERS)
endif()



# Testing
option(APEX_BUILD_TESTS "Build tests" ON)
if(APEX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarking
option(APEX_BUILD_BENCHMARKS "Build benchmarks" ON)
if(APEX_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Examples
option(APEX_BUILD_EXAMPLES "Build examples" ON)
if(APEX_BUILD_EXAMPLES)
    add_subdirectory(examples/trading)
endif()

# Bindings
option(APEX_BUILD_PYTHON "Build Python bindings" ON)
if(APEX_BUILD_PYTHON)
    add_subdirectory(bindings/python)
endif()

option(APEX_BUILD_JAVA "Build Java bindings" ON)
if(APEX_BUILD_JAVA)
    add_subdirectory(bindings/java)
endif()

# Installation
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/dist")
install(TARGETS aarchgate DESTINATION lib)
install(FILES include/apex/apex_c_api.h include/apex/AarchGate.hpp DESTINATION include)
