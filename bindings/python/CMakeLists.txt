# Python Bindings (PyBind11)
set(Python3_FIND_STRATEGY LOCATION)
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

if(Python3_FOUND)
    if(NOT TARGET pybind11::module)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../external/pybind11 pybind11_build)
    endif()

    pybind11_add_module(aarchgate_python aarchgate_pybind.cpp)
    target_include_directories(aarchgate_python PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
    target_link_libraries(aarchgate_python PRIVATE aarchgate)

    # Install
    install(TARGETS aarchgate_python DESTINATION python)
    message(STATUS "Python bindings configured")
else()
    message(WARNING "Python headers not found; skipping aarchgate_python bindings")
endif()
