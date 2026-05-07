cmake_minimum_required(VERSION 3.20)

project(TestApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(
    ../AppKernel
    ${CMAKE_CURRENT_BINARY_DIR}/AppKernel
)

add_executable(
    TestApp
    src/main.cpp
    src/config.cpp
    src/media.cpp
    src/runtime.cpp
    src/scan.cpp
)

target_compile_definitions(
    TestApp
    PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(
    TestApp
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
    TestApp
    PRIVATE
    AppKernel
    setupapi
)

if (MINGW)
    target_link_options(
        TestApp
        PRIVATE
        -static
        -static-libstdc++
        -static-libgcc
    )
endif ()
