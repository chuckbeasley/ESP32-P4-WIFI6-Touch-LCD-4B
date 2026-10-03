# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# esp-hosted 3.0.9 + NimBLE + the toolbox apps add enough include directories that
# the per-object compile command for main.cpp exceeds Windows' 32K CreateProcess
# limit. Force CMake to put the arguments in a response file.
set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS ON CACHE BOOL "" FORCE)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS ON CACHE BOOL "" FORCE)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_INCLUDES ON CACHE BOOL "" FORCE)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES ON CACHE BOOL "" FORCE)
set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-brookesia)

if(TARGET __idf_espressif__mcp-c-sdk AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(__idf_espressif__mcp-c-sdk PRIVATE
        -Wno-error=stringop-truncation
    )
endif()

if(TARGET __idf_espressif__esp-boost AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(__idf_espressif__esp-boost PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=deprecated-variadic-comma-omission>
    )
endif()
