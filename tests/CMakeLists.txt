# Unit tests use Qt's own QTest framework — no external deps.
# Each TU is built as its own executable so CTest can report them
# individually and a single test's crash doesn't take the rest down.

find_package(Qt6 REQUIRED COMPONENTS Test Core Network)

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
