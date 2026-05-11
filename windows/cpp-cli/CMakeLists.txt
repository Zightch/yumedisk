cmake_minimum_required(VERSION 3.20)

project(CppCli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(
    ../BackendCore
    ${CMAKE_CURRENT_BINARY_DIR}/BackendCore
)

add_executable(
    cpp_cli
    src/main.cpp
    src/config.cpp
    src/media.cpp
    src/runtime.cpp
)

set_target_properties(
    cpp_cli
    PROPERTIES
    OUTPUT_NAME "cpp-cli"
)

target_compile_definitions(
    cpp_cli
    PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(
    cpp_cli
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
    cpp_cli
    PRIVATE
    BackendCore
    scan
)

add_custom_command(
    TARGET cpp_cli
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:BackendCore>
    $<TARGET_FILE_DIR:cpp_cli>
)

add_custom_command(
    TARGET cpp_cli
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:AppKernel>
    $<TARGET_FILE_DIR:cpp_cli>
)

if (MINGW)
    target_link_options(
        cpp_cli
        PRIVATE
        -static
        -static-libstdc++
        -static-libgcc
    )
endif ()
