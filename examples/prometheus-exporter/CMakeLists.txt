# Standalone example: build against an INSTALLED libqiftop via find_package.
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libqiftop/prefix
#   cmake --build build
#
# A Prometheus / OpenMetrics exporter for the qiftop-agent's live stream.
# Links only qiftop::qiftop (+ transitively Qt6::Core/Network/DBus) — no Qt
# Widgets, no kernel access. Serves /metrics over HTTP for Prometheus to
# scrape; alerting (incl. "incessantly hogging" via a PromQL `for:` clause)
# is then delegated to Prometheus/Alertmanager.
cmake_minimum_required(VERSION 3.21)
project(qiftop-prometheus-exporter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(qiftop REQUIRED)

add_executable(qiftop-exporter main.cpp)
target_link_libraries(qiftop-exporter PRIVATE qiftop::qiftop)
