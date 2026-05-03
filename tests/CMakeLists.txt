# Unit tests for Apex Engine
# Test infrastructure and placeholder for test targets

# Registry tests
add_executable(test_registry test_registry.cpp)
target_include_directories(test_registry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_registry PRIVATE aarchgate)
target_compile_options(test_registry PRIVATE ${APEX_COMPILE_FLAGS})

# Add generated FlatBuffers headers directory if available
if(flatbuffers_FOUND)
    target_include_directories(test_registry PRIVATE "${FLATBUFFERS_GENERATED_DIR}")
endif()

add_test(NAME RegistryTests COMMAND test_registry)

# Bit-Slicer transpose audit tests
add_executable(test_bit_slicer_transpose test_bit_slicer_transpose.cpp)
target_include_directories(test_bit_slicer_transpose PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_bit_slicer_transpose PRIVATE aarchgate)
target_compile_options(test_bit_slicer_transpose PRIVATE ${APEX_COMPILE_FLAGS})

if(highway_FOUND)
    target_compile_definitions(test_bit_slicer_transpose PRIVATE APEX_HAS_HIGHWAY)
endif()

add_test(NAME BitSlicerTransposeTests COMMAND test_bit_slicer_transpose)

# SDK IoT sensor test
add_executable(test_sdk_iot test_sdk_iot.cpp)
target_include_directories(test_sdk_iot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_sdk_iot PRIVATE aarchgate)
target_compile_options(test_sdk_iot PRIVATE ${APEX_COMPILE_FLAGS})

if(asmjit_FOUND)
    target_compile_definitions(test_sdk_iot PRIVATE APEX_HAS_ASMJIT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../external/asmjit")
        target_include_directories(test_sdk_iot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/asmjit")
    endif()
endif()

if(highway_FOUND)
    target_compile_definitions(test_sdk_iot PRIVATE APEX_HAS_HIGHWAY)
endif()

# Universal C++ SDK test
add_executable(test_universal_cxx test_universal_cxx.cpp)
target_include_directories(test_universal_cxx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_universal_cxx PRIVATE aarchgate)
target_compile_options(test_universal_cxx PRIVATE ${APEX_COMPILE_FLAGS})
add_test(NAME UniversalCXXTests COMMAND test_universal_cxx)



# Infrastructure Audit test
add_executable(test_infrastructure_audit test_infrastructure_audit.cpp)
target_include_directories(test_infrastructure_audit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_infrastructure_audit PRIVATE aarchgate)
target_compile_options(test_infrastructure_audit PRIVATE ${APEX_COMPILE_FLAGS})

if(asmjit_FOUND)
    target_compile_definitions(test_infrastructure_audit PRIVATE APEX_HAS_ASMJIT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../external/asmjit")
        target_include_directories(test_infrastructure_audit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/asmjit")
    endif()
endif()

if(highway_FOUND)
    target_compile_definitions(test_infrastructure_audit PRIVATE APEX_HAS_HIGHWAY)
endif()

add_test(NAME InfrastructureAudit COMMAND test_infrastructure_audit)



# Memory fabric tests (requires iceoryx)
if(iceoryx_FOUND)
    add_executable(test_memory_fabric test_memory_fabric.cpp)
    target_include_directories(test_memory_fabric PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_link_libraries(test_memory_fabric PRIVATE aarchgate iceoryx_posh iceoryx_hoofs iceoryx_posh_roudi iceoryx_posh_config)
    target_compile_options(test_memory_fabric PRIVATE ${APEX_COMPILE_FLAGS})
    target_compile_definitions(test_memory_fabric PRIVATE APEX_HAS_ICEORYX)

    # Add generated FlatBuffers headers directory if available
    if(flatbuffers_FOUND)
        target_include_directories(test_memory_fabric PRIVATE "${FLATBUFFERS_GENERATED_DIR}")
    endif()

    add_test(NAME MemoryFabricTests COMMAND test_memory_fabric)
    message(STATUS "Tests configured: test_registry, test_infrastructure_audit, test_memory_fabric")
else()
    message(STATUS "Tests configured: test_registry, test_infrastructure_audit (test_memory_fabric requires iceoryx)")
endif()
# ML Inference Phase 1 test
add_executable(test_ml_inference test_ml_inference.cpp)
target_include_directories(test_ml_inference PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_ml_inference PRIVATE aarchgate)
target_compile_options(test_ml_inference PRIVATE ${APEX_COMPILE_FLAGS})
add_test(NAME MLInferenceTests COMMAND test_ml_inference)
