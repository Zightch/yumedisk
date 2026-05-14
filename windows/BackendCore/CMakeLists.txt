cmake_minimum_required(VERSION 3.19)
include(env.cmake OPTIONAL)

project(BackendCore LANGUAGES C CXX)

add_subdirectory(
        ../AppKernel
        ${CMAKE_CURRENT_BINARY_DIR}/AppKernel
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(
        BackendCore
        SHARED
        BackendCore/BackendCore.h
        config/config.cpp
        config/config.h
        media/media.cpp
        media/media.h
        media/Media/Media.h
        runtime/runtime.cpp
        runtime/runtime.h
        runtime/runtimeDisk.h
        StagingStore/StagingStore.cpp
        StagingStore/StagingStore.h
        types/types.h
)

target_compile_definitions(
        BackendCore
        PRIVATE
        BACKENDCORE_BUILD_SHARED
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

target_include_directories(
        BackendCore
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/BackendCore
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
        BackendCore
        PRIVATE
        AppKernel
)

add_custom_command(
        TARGET BackendCore
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:AppKernel>
        $<TARGET_FILE_DIR:BackendCore>
)
