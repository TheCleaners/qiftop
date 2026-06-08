# Unit tests use Qt's own QTest framework — no external deps.
# Each TU is built as its own executable so CTest can report them
# individually and a single test's crash doesn't take the rest down.

find_package(Qt6 REQUIRED COMPONENTS Test Core Network DBus Widgets)

function(qiftop_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Network Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

qiftop_add_test(test_direction        test_direction.cpp)
qiftop_add_test(test_forwarded        test_forwarded.cpp)
qiftop_add_test(test_ema              test_ema.cpp)
qiftop_add_test(test_settings_migration
    test_settings_migration.cpp
    ${CMAKE_SOURCE_DIR}/src/config/Settings.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Autostart.cpp)
qiftop_add_test(test_autostart
    test_autostart.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Autostart.cpp)
qiftop_add_test(test_filter
    test_filter.cpp
    ${CMAKE_SOURCE_DIR}/src/util/ConnectionFilter.cpp)

qiftop_add_test(test_exporter
    test_exporter.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Exporter.cpp)

qiftop_add_test(test_idle
    test_idle.cpp
    ${CMAKE_SOURCE_DIR}/src/agent/IdleManager.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/ConnectionMonitor.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/NetworkMonitor.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Logging.cpp)
target_link_libraries(test_idle PRIVATE Qt6::DBus)

qiftop_add_test(test_agent_config
    test_agent_config.cpp
    ${CMAKE_SOURCE_DIR}/src/agent/Config.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Logging.cpp)
target_link_libraries(test_agent_config PRIVATE Qt6::DBus)

qiftop_add_test(test_dbus_types
    test_dbus_types.cpp
    ${CMAKE_SOURCE_DIR}/src/dbus/Types.cpp)
target_link_libraries(test_dbus_types PRIVATE Qt6::DBus)

qiftop_add_test(test_dns_cache
    test_dns_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/dns/DnsResolver.cpp
    ${CMAKE_SOURCE_DIR}/src/dns/QtDnsResolver.cpp)

qiftop_add_test(test_handoff_auth
    test_handoff_auth.cpp
    ${CMAKE_SOURCE_DIR}/src/util/HandoffServer.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Logging.cpp)

qiftop_add_test(test_proxies
    test_proxies.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ConnectionFilterProxy.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/InterfaceFilterProxy.cpp
    ${CMAKE_SOURCE_DIR}/src/util/ConnectionFilter.cpp)
target_link_libraries(test_proxies PRIVATE Qt6::Widgets)

# Integration test: spawns the actual qiftop-agent binary against a session
# bus and drives the live DBus contract end-to-end. Slow (~6 s; baseline +
# fast windows + spawn/teardown per test method) so it's gated behind
# QIFTOP_BUILD_INTEGRATION_TESTS=ON. CI enables it explicitly.
option(QIFTOP_BUILD_INTEGRATION_TESTS
       "Build integration tests (require a running session DBus)" ON)
if(QIFTOP_BUILD_INTEGRATION_TESTS)
    qiftop_add_test(test_agent_integration test_agent_integration.cpp)
    target_link_libraries(test_agent_integration PRIVATE Qt6::DBus)
    target_compile_definitions(test_agent_integration PRIVATE
        "QIFTOP_AGENT_BINARY=\"$<TARGET_FILE:qiftop-agent>\"")
    add_dependencies(test_agent_integration qiftop-agent)
endif()
