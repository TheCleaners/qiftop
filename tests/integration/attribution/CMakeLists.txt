# Attribution Tier-2 integration harness.
#
# Built only when QIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON (default OFF) AND
# the platform is Linux. The probe binary is NOT registered with ctest by
# default — it is driven by the shell runners under runners/, themselves
# invoked manually from the devbox (scripts/integration-test.sh) or by
# .github/workflows/integration.yml on push-to-main / release / dispatch.
#
# Rationale: probe needs CAP_NET_ADMIN + CAP_SYS_ADMIN AND a working
# container runtime to do anything meaningful, so wiring it into the
# default ctest set would either fail silently or require sudo for every
# `make test` invocation.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

add_executable(qiftop-attribution-probe
    driver/qiftop-attribution-probe.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/ProcessResolverFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/PlatformInfo.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Logging.cpp
)

target_include_directories(qiftop-attribution-probe PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(qiftop-attribution-probe PRIVATE
    Qt6::Core
    Qt6::Network
    backend_linux
)

target_compile_definitions(qiftop-attribution-probe PRIVATE BACKEND_LINUX)
qiftop_apply_attribution_defs(qiftop-attribution-probe)

# Register a ctest entry per runtime, all labelled "attribution-integration".
# Each runner script knows how to bring up its runtime, drive the probe,
# and tear down. Exit 77 = ctest SKIP (runtime missing or unhealthy).
foreach(runtime docker podman)
    add_test(
        NAME    attribution_${runtime}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/runners/run-${runtime}.sh
    )
    set_tests_properties(attribution_${runtime} PROPERTIES
        LABELS "attribution-integration"
        ENVIRONMENT "QIFTOP_PROBE_BIN=$<TARGET_FILE:qiftop-attribution-probe>"
        TIMEOUT 90
        SKIP_RETURN_CODE 77
    )
endforeach()
