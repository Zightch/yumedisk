cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(RWTestApp)

add_subdirectory(
    ../../AppKernel
    ${CMAKE_CURRENT_BINARY_DIR}/AppKernel
)

add_executable(
    RWTestApp
    main.cpp
)

target_include_directories(
    RWTestApp
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

target_link_libraries(
    RWTestApp
    PRIVATE
    AppKernel
    setupapi
)

if (MINGW)
    target_link_options(
        RWTestApp
        PRIVATE
        -static
        -static-libstdc++
        -static-libgcc
    )
endif()
