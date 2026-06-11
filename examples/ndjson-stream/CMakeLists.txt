# Standalone example: build against an INSTALLED libqiftop via find_package.
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libqiftop/prefix
#   cmake --build build
#
# Demonstrates that libqiftop is a self-contained, Widgets-free data
# facility: this consumer links only qiftop::qiftop (+ transitively
# Qt6::Core/Network/DBus) and turns the agent's DBus stream into NDJSON.
cmake_minimum_required(VERSION 3.21)
project(qiftop-ndjson-example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(qiftop REQUIRED)

add_executable(qiftop-ndjson main.cpp)
target_link_libraries(qiftop-ndjson PRIVATE qiftop::qiftop)
