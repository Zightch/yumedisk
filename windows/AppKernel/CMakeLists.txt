cmake_minimum_required(VERSION 3.20)

project(AppKernel LANGUAGES C)

add_library(
    AppKernel
    SHARED
    src/appkernel.c
    src/common/ak_memory.c
    src/common/ak_status.c
    src/disk/ak_disk.c
    src/event/ak_event.c
    src/protocol/ak_protocol.c
    src/session/ak_session.c
)

target_compile_features(AppKernel PRIVATE c_std_11)

target_compile_definitions(
    AppKernel
    PRIVATE
    AK_BUILD_DLL
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(
    AppKernel
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(
    AppKernel
    PROPERTIES
    OUTPUT_NAME AppKernel
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

target_link_libraries(
    AppKernel
    PRIVATE
    setupapi
)

if (MSVC)
    target_compile_options(
        AppKernel
        PRIVATE
        /W4
        /wd4201
    )
else ()
    target_compile_options(
        AppKernel
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif ()
