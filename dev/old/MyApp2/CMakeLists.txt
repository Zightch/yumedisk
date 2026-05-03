cmake_minimum_required(VERSION 3.10)

set(ProjectName MyApp2)
set(CMAKE_CXX_STANDARD 11)

project(${ProjectName})

add_executable(
    ${ProjectName}
    main.cpp
)

target_link_libraries(
    ${ProjectName}
    PRIVATE
    -static
    -static-libstdc++
    -static-libgcc
)
