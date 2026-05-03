cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(MyApp5)

include_directories(
        utils
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)
add_executable(
        MyApp5
        main.cpp
        utils/utils.cpp
)

if (MINGW)
    target_link_options(
            MyApp5
            PRIVATE
            -static
            -static-libstdc++
            -static-libgcc
    )
endif()

target_link_libraries(
        MyApp5
        PRIVATE
        setupapi
)
