cmake_minimum_required(VERSION 3.20)

project(scan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(
    scan
    STATIC
    scan.cpp
    scan.h
)

target_compile_definitions(
    scan
    PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(
    scan
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    scan
    PUBLIC
    setupapi
)
