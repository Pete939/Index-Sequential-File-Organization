cmake_minimum_required(VERSION 4.0)
project(p2)

set(CMAKE_CXX_STANDARD 20)

find_package(fmt REQUIRED)
add_executable(p2 main.cpp
        Record.h)
target_link_libraries(p2 PRIVATE fmt::fmt)
