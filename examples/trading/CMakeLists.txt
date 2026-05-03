# Trading Example for Apex Engine

add_executable(trading_example main.cpp)
target_include_directories(trading_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_link_libraries(trading_example PRIVATE aarchgate)

if(asmjit_FOUND)
    target_include_directories(trading_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../external/asmjit)
endif()

# FlatBuffers Generation
set(FLATBUFFERS_SCHEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/schema")
set(FLATBUFFERS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(flatbuffers_FOUND)
    file(MAKE_DIRECTORY "${FLATBUFFERS_GENERATED_DIR}")
    add_custom_command(
        OUTPUT "${FLATBUFFERS_GENERATED_DIR}/market_data_generated.h"
        COMMAND flatc -c --gen-onefile -o "${FLATBUFFERS_GENERATED_DIR}" "${FLATBUFFERS_SCHEMA_DIR}/market_data.fbs"
        DEPENDS "${FLATBUFFERS_SCHEMA_DIR}/market_data.fbs"
        COMMENT "Generating FlatBuffers C++ headers from market_data.fbs"
    )
    add_custom_target(trading_flatbuffers_gen ALL
        DEPENDS "${FLATBUFFERS_GENERATED_DIR}/market_data_generated.h"
    )
    target_include_directories(trading_example PRIVATE "${FLATBUFFERS_GENERATED_DIR}")
    add_dependencies(trading_example trading_flatbuffers_gen)
endif()

# Tests
add_executable(test_universal_logic test_universal_logic.cpp)
target_include_directories(test_universal_logic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_link_libraries(test_universal_logic PRIVATE aarchgate)
add_test(NAME TradingUniversalLogic COMMAND test_universal_logic)

add_executable(test_hybrid_sdk test_hybrid_sdk.cpp)
target_include_directories(test_hybrid_sdk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_link_libraries(test_hybrid_sdk PRIVATE aarchgate)
add_test(NAME TradingHybridSDK COMMAND test_hybrid_sdk)

# Benchmark
add_executable(bench_10b_rps bench_10b_rps.cpp)
target_include_directories(bench_10b_rps PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_link_libraries(bench_10b_rps PRIVATE aarchgate)
