# Java Bindings (JNI)
find_package(JNI REQUIRED)
find_package(Java REQUIRED COMPONENTS Development)

if(JNI_FOUND)
    add_library(aarchgate_java SHARED aarchgate_jni.cpp)
    target_include_directories(aarchgate_java PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
    target_include_directories(aarchgate_java PRIVATE ${JNI_INCLUDE_DIRS})
    target_link_libraries(aarchgate_java PRIVATE aarchgate)

    # Enable LTO for the JNI wrapper as well
    set_target_properties(aarchgate_java PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)

    # Compile Java file
    find_package(Java COMPONENTS Development QUIET)
    if(Java_FOUND)
        set(JAVA_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/classes")
        file(MAKE_DIRECTORY ${JAVA_OUT_DIR})

        add_custom_command(
            OUTPUT "${JAVA_OUT_DIR}/com/aarchgate/engine/AarchGateEngine.class"
            COMMAND ${Java_JAVAC_EXECUTABLE} -d ${JAVA_OUT_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/com/aarchgate/engine/AarchGateEngine.java"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/com/aarchgate/engine/AarchGateEngine.java"
            COMMENT "Compiling Java bindings"
        )

        add_custom_target(aarchgate_java_classes ALL
            DEPENDS "${JAVA_OUT_DIR}/com/aarchgate/engine/AarchGateEngine.class"
        )
    endif()

    # Install
    install(TARGETS aarchgate_java DESTINATION lib)
else()
    message(STATUS "JNI not found; skipping Java bindings")
endif()
