# (c) 2024-2026 Suprath PS. All rights reserved.

add_executable(bench_random_forest bench_random_forest.cpp apex_runner_ext.cpp)
target_link_libraries(bench_random_forest PRIVATE aarchgate)
target_compile_options(bench_random_forest PRIVATE ${APEX_COMPILE_FLAGS} -fno-lto)
set_target_properties(bench_random_forest PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(bench_native bench_native.cpp)
target_link_libraries(bench_native PRIVATE aarchgate)
target_compile_options(bench_native PRIVATE ${APEX_COMPILE_FLAGS})

# Disable LTO explicitly for runner object
set_source_files_properties(apex_runner_ext.cpp PROPERTIES COMPILE_FLAGS "-fno-lto")
