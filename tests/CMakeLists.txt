# Unit tests use Qt's own QTest framework — no external deps.
# Each TU is built as its own executable so CTest can report them
# individually and a single test's crash doesn't take the rest down.

find_package(Qt6 REQUIRED COMPONENTS Test Core Network DBus Widgets)

# Sanitizer support for tests only (production builds stay clean).
#
# Accepted values:
#   OFF              — no sanitizers (default)
#   address          — AddressSanitizer
#   undefined        — UndefinedBehaviorSanitizer
#   address+undefined — both (recommended for CI; ~2× slower, finds more bugs)
#   thread           — ThreadSanitizer (incompatible with address)
#   leak             — LeakSanitizer standalone (subset of address)
#
# Use:   cmake -B build -DQIFTOP_TESTS_SANITIZE=address+undefined
# Then:  ctest --test-dir build --output-on-failure
#
# Only GCC and Clang are supported. The sanitizers are applied per-test-target
# (compile + link flags) so that production binaries (`qiftop`, `qiftop-agent`)
# remain unaffected and any package built from the same tree is shippable.
set(QIFTOP_TESTS_SANITIZE "OFF" CACHE STRING
    "Sanitizers to enable for test targets (OFF, address, undefined, address+undefined, thread, leak)")
set_property(CACHE QIFTOP_TESTS_SANITIZE PROPERTY STRINGS
    OFF address undefined address+undefined thread leak)

set(_qiftop_san_compile "")
set(_qiftop_san_link "")
if(NOT QIFTOP_TESTS_SANITIZE STREQUAL "OFF")
    if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        message(FATAL_ERROR
            "QIFTOP_TESTS_SANITIZE requires GCC or Clang (got ${CMAKE_CXX_COMPILER_ID})")
    endif()
    set(_san_list "")
    if(QIFTOP_TESTS_SANITIZE STREQUAL "address")
        list(APPEND _san_list "address")
    elseif(QIFTOP_TESTS_SANITIZE STREQUAL "undefined")
        list(APPEND _san_list "undefined")
    elseif(QIFTOP_TESTS_SANITIZE STREQUAL "address+undefined")
        list(APPEND _san_list "address" "undefined")
    elseif(QIFTOP_TESTS_SANITIZE STREQUAL "thread")
        list(APPEND _san_list "thread")
    elseif(QIFTOP_TESTS_SANITIZE STREQUAL "leak")
        list(APPEND _san_list "leak")
    else()
        message(FATAL_ERROR
            "Unknown QIFTOP_TESTS_SANITIZE value '${QIFTOP_TESTS_SANITIZE}' "
            "(expected one of: OFF, address, undefined, address+undefined, thread, leak)")
    endif()
    string(REPLACE ";" "," _san_csv "${_san_list}")
    # -fno-omit-frame-pointer keeps stack traces readable;
    # -g ensures symbols even in Release builds.
    set(_qiftop_san_compile -fsanitize=${_san_csv} -fno-omit-frame-pointer -g)
    set(_qiftop_san_link    -fsanitize=${_san_csv})
    message(STATUS "qiftop: test targets built with -fsanitize=${_san_csv}")
endif()

function(qiftop_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Network Qt6::Test)
    if(_qiftop_san_compile)
        target_compile_options(${name} PRIVATE ${_qiftop_san_compile})
        target_link_options(${name}    PRIVATE ${_qiftop_san_link})
    endif()
    add_test(NAME ${name} COMMAND ${name})
    if(_qiftop_san_compile)
        # Make sanitizer diagnostics useful in CI: full backtraces, treat
        # leaks as errors, ASan needs detect_leaks=1 to actually run LSan.
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT
                "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:symbolize=1:strict_string_checks=1:detect_stack_use_after_return=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1;LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/lsan.supp:print_suppressions=0")
    endif()
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

qiftop_add_test(test_units test_units.cpp)

qiftop_add_test(test_priv_escalator
    test_priv_escalator.cpp
    ${CMAKE_SOURCE_DIR}/src/util/PrivilegeEscalator.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Logging.cpp)

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
