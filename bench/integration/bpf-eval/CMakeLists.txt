# conntrack-vs-BPF/pcap capture measurement harness — Phase 0 (conntrack baseline).
#
# SKIP-SAFE BY DESIGN: this file is only ever processed when
# -DQIFTOP_BUILD_BPF_EVAL=ON, and even then it refuses to FAIL configure when a
# prerequisite is missing — it skips the affected target with a STATUS message.
# The generator + sink are plain-socket C++ (no deps, always build). The
# conntrack collector reuses the production ConntrackMonitor (qiftoplib /
# backend_linux) and is skipped if that target isn't available. The scorer +
# orchestrator are scripts (no build step).

message(STATUS "bpf-eval: building the conntrack-vs-BPF capture measurement harness (Phase 0)")

# --- Ground-truth traffic generator + sink (no special deps) ----------------
add_executable(qiftop-bpfeval-sink sink.cpp)
add_executable(qiftop-bpfeval-gen  gen.cpp)
foreach(_t qiftop-bpfeval-sink qiftop-bpfeval-gen)
    set_target_properties(${_t} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bpf-eval")
endforeach()

# --- conntrack collector (reuses the REAL ConntrackMonitor) -----------------
# Gate on the Linux backend actually being present so a misconfigured tree (or
# a future non-conntrack Linux build) skips rather than errors.
if(TARGET qiftoplib AND QIFTOP_BACKEND_TARGET STREQUAL "backend_linux")
    add_executable(qiftop-bpfeval-conntrack conntrack_collect.cpp)
    target_link_libraries(qiftop-bpfeval-conntrack PRIVATE qiftoplib Qt6::Core Qt6::Network)
    target_include_directories(qiftop-bpfeval-conntrack PRIVATE ${CMAKE_SOURCE_DIR}/src)
    set_target_properties(qiftop-bpfeval-conntrack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bpf-eval")
else()
    message(STATUS "bpf-eval: Linux conntrack backend unavailable — skipping conntrack collector "
                   "(generator + sink still built; pcap/eBPF collectors are later phases)")
endif()

# Stage the scripts next to the binaries so the orchestrator finds everything
# in one place.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/score.py
               ${CMAKE_BINARY_DIR}/bpf-eval/score.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/run-bpf-eval.sh
               ${CMAKE_BINARY_DIR}/bpf-eval/run-bpf-eval.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/birth.bt
               ${CMAKE_BINARY_DIR}/bpf-eval/birth.bt COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cost-bpf-eval.sh
               ${CMAKE_BINARY_DIR}/bpf-eval/cost-bpf-eval.sh COPYONLY)
