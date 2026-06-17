# Performance benchmarks use Qt Test's QBENCHMARK macros and are opt-in via
# QIFTOP_BUILD_BENCHMARKS. They are deliberately EXCLUDE_FROM_ALL and disabled
# in CTest so normal builds and default ctest runs remain unaffected.

find_package(Qt6 REQUIRED COMPONENTS Test Core Network DBus)

function(qiftop_add_benchmark name)
    add_executable(${name} EXCLUDE_FROM_ALL ${ARGN})
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/bench)
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Network Qt6::Test)

    list(APPEND QIFTOP_BENCH_TARGETS ${name})
    set(QIFTOP_BENCH_TARGETS "${QIFTOP_BENCH_TARGETS}" PARENT_SCOPE)

    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS benchmark DISABLED TRUE)
endfunction()

qiftop_add_benchmark(bench_connection_aggregator
    bench_connection_aggregator.cpp
    ${CMAKE_SOURCE_DIR}/src/aggregate/ConnectionAggregator.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/PlatformInfo.cpp
    ${CMAKE_SOURCE_DIR}/src/dns/DnsResolver.cpp)

qiftop_add_benchmark(bench_filter
    bench_filter.cpp
    ${CMAKE_SOURCE_DIR}/src/util/ConnectionFilter.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    qiftop_add_benchmark(bench_flow_topk
        bench_flow_topk.cpp)

    # The "eager budget" bench: top-K cap + aggregator + filter in one tick.
    # Linux-gated because it pulls in FlowTopK.h (the in-process conntrack cap).
    qiftop_add_benchmark(bench_pipeline_tick
        bench_pipeline_tick.cpp
        ${CMAKE_SOURCE_DIR}/src/aggregate/ConnectionAggregator.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/PlatformInfo.cpp
        ${CMAKE_SOURCE_DIR}/src/dns/DnsResolver.cpp
        ${CMAKE_SOURCE_DIR}/src/util/ConnectionFilter.cpp)
endif()

add_custom_target(benchmarks DEPENDS ${QIFTOP_BENCH_TARGETS})
