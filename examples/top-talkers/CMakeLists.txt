# Standalone example: build against an INSTALLED libqiftop via find_package.
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libqiftop/prefix
#   cmake --build build
#
# Headless "iftop -t"-style top-talkers printer.
cmake_minimum_required(VERSION 3.21)
project(qiftop-top-talkers-example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(qiftop REQUIRED)

add_executable(qiftop-top main.cpp)
target_link_libraries(qiftop-top PRIVATE qiftop::qiftop)
