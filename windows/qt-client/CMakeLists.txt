cmake_minimum_required(VERSION 3.19)
include(env.cmake OPTIONAL)

project(qt-client LANGUAGES CXX)

add_subdirectory(
        ../BackendCore
        ${CMAKE_CURRENT_BINARY_DIR}/BackendCore
)

find_package(Qt6 6 REQUIRED COMPONENTS Core Widgets)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

qt_standard_project_setup()

qt_add_executable(
        qt-client
        CreateDiskDialog/CreateDiskDialog.cpp
        CreateDiskDialog/CreateDiskDialog.h
        CreateDiskDialog/CreateDiskDialog.ui
        backendHost/BackendHost/BackendHost.cpp
        backendHost/BackendHost/BackendHost.h
        main.cpp
        media/FileMedia/FileMedia.cpp
        media/FileMedia/FileMedia.h
        media/MemoryMedia/MemoryMedia.cpp
        media/MemoryMedia/MemoryMedia.h
        media/RawFileMedia/RawFileMedia.cpp
        media/RawFileMedia/RawFileMedia.h
        Widget/Widget.cpp
        Widget/Widget.h
        Widget/Widget.ui
)

target_include_directories(
        qt-client
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
        qt-client
        PRIVATE
        BackendCore
        Qt::Core
        Qt::Widgets
)

add_custom_command(
        TARGET qt-client
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:BackendCore>
        $<TARGET_FILE_DIR:qt-client>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:AppKernel>
        $<TARGET_FILE_DIR:qt-client>
)

set_target_properties(qt-client PROPERTIES WIN32_EXECUTABLE TRUE)
