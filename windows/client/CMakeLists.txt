cmake_minimum_required(VERSION 3.19)
include(env.cmake OPTIONAL)

project(client LANGUAGES CXX)

add_subdirectory(
        ../AppKernel
        ${CMAKE_CURRENT_BINARY_DIR}/AppKernel
)

add_subdirectory(
        ../scan
        ${CMAKE_CURRENT_BINARY_DIR}/scan
)

find_package(Qt6 6 REQUIRED COMPONENTS Core Widgets)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

qt_standard_project_setup()

include_directories(./)

qt_add_executable(
        client
        main.cpp
        widget.cpp
        widget.h
        widget.ui
)

target_link_libraries(
        client
        PRIVATE
        AppKernel
        scan
        Qt::Core
        Qt::Widgets
)

set_target_properties(client PROPERTIES WIN32_EXECUTABLE TRUE)
