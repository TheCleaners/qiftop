cmake_minimum_required(VERSION 3.21)
project(qiftop VERSION 0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network DBus)

# --- Per-flow process / container attribution (v0.2+) -----------------------
# Three independent compile-time toggles. Each gates a layer of the
# ProcessResolver pipeline; absent flags mean the corresponding source
# files are simply not compiled (and the resolver layer is unavailable
# at runtime, not greyed-out in the UI — the UI follows the runtime
# Capabilities token list). Defaults: ON on Linux, OFF elsewhere.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(QIFTOP_ATTRIBUTION_DEFAULT ON)
else()
    set(QIFTOP_ATTRIBUTION_DEFAULT OFF)
endif()
option(QIFTOP_ENABLE_PROCESS_ATTRIBUTION
    "Enable per-flow process attribution (NETLINK_SOCK_DIAG + /proc walk on Linux)"
    ${QIFTOP_ATTRIBUTION_DEFAULT})
option(QIFTOP_ENABLE_CONTAINER_ATTRIBUTION
    "Enable per-flow container attribution (cgroup classifier on Linux)"
    ${QIFTOP_ATTRIBUTION_DEFAULT})
option(QIFTOP_ENABLE_NETNS_SCAN
    "Enable cross-network-namespace socket scanning (setns + sock_diag; Linux only, needs CAP_SYS_ADMIN)"
    ${QIFTOP_ATTRIBUTION_DEFAULT})

# Expose the toggles to all targets as compile definitions, so the
# resolver factory and concrete impls can #ifdef on them. Non-Linux
# builds get NONE of these defines regardless of the user's setting.
function(qiftop_apply_attribution_defs target)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    if(QIFTOP_ENABLE_PROCESS_ATTRIBUTION)
        target_compile_definitions(${target} PRIVATE QIFTOP_ENABLE_PROCESS_ATTRIBUTION)
    endif()
    if(QIFTOP_ENABLE_CONTAINER_ATTRIBUTION)
        target_compile_definitions(${target} PRIVATE QIFTOP_ENABLE_CONTAINER_ATTRIBUTION)
    endif()
    if(QIFTOP_ENABLE_NETNS_SCAN)
        target_compile_definitions(${target} PRIVATE QIFTOP_ENABLE_NETNS_SCAN)
    endif()
endfunction()

# --- Shared DBus type marshalling (used by both client and agent) -----------
add_library(qiftop_dbus STATIC
    src/dbus/Types.cpp
)
target_include_directories(qiftop_dbus PUBLIC src)
target_link_libraries(qiftop_dbus PUBLIC Qt6::Core Qt6::Network Qt6::DBus)

# --- Client (qiftop) --------------------------------------------------------
# qiftop_ui is a static library carrying every translation unit of the GUI
# client EXCEPT main.cpp. Both the qiftop executable and the widget-level
# smoke tests (tests/test_mainwindow_smoke.cpp) link against it so the
# tests exercise the real production widget tree, models, and proxies —
# not stand-in lookalikes.
add_library(qiftop_ui STATIC
    src/backend/NetworkMonitor.cpp
    src/backend/ConnectionMonitor.cpp
    src/backend/PlatformInfo.cpp
    src/backend/ProcessResolverFactory.cpp
    src/backend/dbus/DBusNetworkMonitor.cpp
    src/backend/dbus/DBusConnectionMonitor.cpp
    src/config/Settings.cpp
    src/dns/DnsResolver.cpp
    src/dns/QtDnsResolver.cpp
    src/ui/MainWindow.cpp
    src/ui/NetworkModel.cpp
    src/ui/InterfaceFilterProxy.cpp
    src/ui/InterfaceNameDelegate.cpp
    src/ui/ConnectionModel.cpp
    src/ui/ConnectionFilterProxy.cpp
    src/ui/ConnectionGroupProxy.cpp
    src/ui/ConnectionFlowDelegate.cpp
    src/ui/RowGaugeDelegate.cpp
    src/ui/ConnectionAttributionDelegate.cpp
    src/ui/SettingsDialog.cpp
    src/ui/TrayManager.cpp
    src/util/Autostart.cpp
    src/util/ConnectionFilter.cpp
    src/util/Exporter.cpp
    src/util/HandoffClient.cpp
    src/util/HandoffServer.cpp
    src/util/Logging.cpp
    src/util/PrivilegeEscalator.cpp
)
target_include_directories(qiftop_ui PUBLIC src)
target_link_libraries(qiftop_ui PUBLIC
    Qt6::Core Qt6::Widgets Qt6::Network Qt6::DBus qiftop_dbus)

add_executable(qiftop src/main.cpp)
target_link_libraries(qiftop PRIVATE qiftop_ui)
# Single source of truth for the release version: derive the runtime
# --version string from the CMake project() version instead of hardcoding it
# in main.cpp (which silently drifted from 0.1 to 0.2).
target_compile_definitions(qiftop PRIVATE QIFTOP_VERSION="${PROJECT_VERSION}")

# Platform backend selection (shared between client and agent)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(src/backend/linux)
    target_link_libraries(qiftop_ui PUBLIC backend_linux)
    target_compile_definitions(qiftop_ui PUBLIC BACKEND_LINUX)
else()
    message(FATAL_ERROR "No backend available for platform: ${CMAKE_SYSTEM_NAME}")
endif()

qiftop_apply_attribution_defs(qiftop_ui)

# --- Privileged agent (qiftop-agent) ----------------------------------------
add_executable(qiftop-agent
    src/agent/main.cpp
    src/agent/Application.cpp
    src/agent/Config.cpp
    src/agent/InterfacesService.cpp
    src/agent/ConnectionsService.cpp
    src/agent/Attribution.cpp
    src/agent/IdleManager.cpp
    src/backend/NetworkMonitor.cpp
    src/backend/ConnectionMonitor.cpp
    src/backend/PlatformInfo.cpp
    src/backend/ProcessResolverFactory.cpp
    src/util/Logging.cpp
)
target_include_directories(qiftop-agent PRIVATE src)
target_link_libraries(qiftop-agent PRIVATE Qt6::Core Qt6::DBus Qt6::Network qiftop_dbus)
target_compile_definitions(qiftop-agent PRIVATE QIFTOP_VERSION="${PROJECT_VERSION}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(qiftop-agent PRIVATE backend_linux)
    target_compile_definitions(qiftop-agent PRIVATE BACKEND_LINUX)
endif()
qiftop_apply_attribution_defs(qiftop-agent)

# --- Demo harness (privacy-safe synthetic GUI driver) -----------------------
# Off by default. Builds docs/demo/qiftop-demo.cpp, which drives the real
# MainWindow from the in-memory test fakes with fabricated data (RFC 5737
# doc IPs + RFC 2606 example.* hostnames) for generating the README capture.
option(QIFTOP_BUILD_DEMO "Build the synthetic GUI demo harness (docs/demo)" OFF)
if(QIFTOP_BUILD_DEMO)
    add_executable(qiftop-demo
        docs/demo/qiftop-demo.cpp
        tests/fakes/FakeMonitors.h)
    target_link_libraries(qiftop-demo PRIVATE qiftop_ui Qt6::Widgets)
    # The fakes live under tests/ and are included as "fakes/FakeMonitors.h".
    target_include_directories(qiftop-demo PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
    qiftop_apply_attribution_defs(qiftop-demo)
endif()

# --- Install rules ----------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS qiftop
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT qiftop)

install(TARGETS qiftop-agent
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT qiftop-agent)

install(FILES LICENSE README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        COMPONENT qiftop)

# Desktop entry + scalable icon (hicolor theme)
install(FILES dist/desktop/qiftop.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        COMPONENT qiftop)
install(FILES dist/desktop/qiftop.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        COMPONENT qiftop)
install(FILES LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/qiftop-agent
        COMPONENT qiftop-agent)

# DBus system bus policy
install(FILES dist/dbus/org.qiftop.NetworkAgent1.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d
        COMPONENT qiftop-agent)

# DBus system-bus activation file
install(FILES dist/dbus/org.qiftop.NetworkAgent1.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system-services
        COMPONENT qiftop-agent)

# systemd unit (absolute path — must be picked up by systemd regardless of
# CMAKE_INSTALL_PREFIX; pkg-config could be used for the systemd-detected
# location but /lib/systemd/system is correct on every Debian-derived distro).
install(FILES dist/systemd/qiftop-agent.service
        DESTINATION /lib/systemd/system
        COMPONENT qiftop-agent)

# Default configuration file. Marked as a Debian conffile (see
# dist/debian/conffiles) so dpkg will preserve user edits across upgrades.
install(FILES dist/conf/agent.conf
        DESTINATION /etc/qiftop
        COMPONENT qiftop-agent)

# nftables shim that the agent's systemd unit loads via ExecStartPre to
# force per-family conntrack on (otherwise IPv6 flows never appear on
# hosts whose firewall rules don't reference `ct` on the ip6 side).
install(FILES dist/nftables/qiftop-conntrack.nft
        DESTINATION share/qiftop
        COMPONENT qiftop-agent)

# --- CPack: produce one .deb per component ----------------------------------
set(CPACK_PACKAGE_NAME              "qiftop")
set(CPACK_PACKAGE_VENDOR            "qiftop project")
set(CPACK_PACKAGE_CONTACT           "qiftop maintainers <noreply@example.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Qt6 iftop-style network monitor")
set(CPACK_PACKAGE_HOMEPAGE_URL      "https://github.com/ebenali/qiftop")
# Strip ELF binaries during packaging — lintian flags unstripped binaries
# as an error, and stripped binaries are ~10× smaller on disk. Symbol
# files for debugging are still available in the build/ tree.
# Overridable: pre-release tags (release.yml) set this to OFF so alpha/rc
# .debs ship with full symbols for crash-report debuggability.
if(NOT DEFINED CPACK_STRIP_FILES)
    set(CPACK_STRIP_FILES TRUE)
endif()
set(CPACK_RESOURCE_FILE_LICENSE     "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README      "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")

set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)
# One file per component, named e.g. qiftop_0.1_amd64.deb
set(CPACK_DEBIAN_FILE_NAME          "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS  ON)
set(CPACK_DEBIAN_PACKAGE_SECTION    "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")

# --- GUI package ------------------------------------------------------------
set(CPACK_DEBIAN_QIFTOP_PACKAGE_NAME         "qiftop")
set(CPACK_DEBIAN_QIFTOP_PACKAGE_DEPENDS      "libc6, libqt6core6t64 | libqt6core6, libqt6widgets6, libqt6network6, libqt6dbus6")
set(CPACK_DEBIAN_QIFTOP_PACKAGE_RECOMMENDS   "qiftop-agent (= ${PROJECT_VERSION})")
set(CPACK_DEBIAN_QIFTOP_DESCRIPTION
"Qt6 iftop-style network monitor (GUI).
qiftop is a Qt 6 desktop application that displays live per-interface
traffic counters and per-connection flows. It cooperates with the
qiftop-agent system service for privileged data access but can also
fall back to running with elevated privileges on demand.")

# --- Agent package ----------------------------------------------------------
set(CPACK_DEBIAN_QIFTOP-AGENT_PACKAGE_NAME       "qiftop-agent")
set(CPACK_DEBIAN_QIFTOP-AGENT_PACKAGE_DEPENDS    "libc6, libnl-3-200, libnl-route-3-200, libnetfilter-conntrack3, libqt6core6t64 | libqt6core6, libqt6network6, libqt6dbus6, dbus")
# nftables is used by the systemd unit's ExecStartPre to wire up per-family
# conntrack so IPv6 flows actually show up. Soft-recommend so the package
# still installs on hosts that don't have it (the unit fails the
# ExecStartPre silently in that case and you just don't get v6 visibility).
set(CPACK_DEBIAN_QIFTOP-AGENT_PACKAGE_RECOMMENDS "nftables")
set(CPACK_DEBIAN_QIFTOP-AGENT_DESCRIPTION
"Privileged DBus agent for qiftop.
qiftop-agent is a small system-bus daemon that publishes per-interface
and per-connection network statistics on the well-known DBus name
org.qiftop.NetworkAgent1. It is activated on demand and runs as root
with CAP_NET_ADMIN only.")

# Run systemctl/dbus reload hooks on install/remove + declare /etc conffiles.
set(CPACK_DEBIAN_QIFTOP-AGENT_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/dist/debian/postinst;${CMAKE_SOURCE_DIR}/dist/debian/postrm;${CMAKE_SOURCE_DIR}/dist/debian/conffiles")

include(CPack)
cpack_add_component(qiftop
    DISPLAY_NAME "qiftop GUI"
    DESCRIPTION  "Qt6 iftop-style network monitor (GUI)")
cpack_add_component(qiftop-agent
    DISPLAY_NAME "qiftop privileged agent"
    DESCRIPTION  "DBus system-bus daemon exposing privileged network statistics")

# --- Auto-rebuild .deb packages on every build ------------------------------
# Convenience for developers: keep the per-component .debs in the build dir
# always in sync with the freshly-linked binaries. Disable with
#   cmake -DQIFTOP_AUTO_PACKAGE=OFF
# in clean-room / CI builds that drive cpack themselves.
#
# Implementation note: we run cpack via add_custom_command(TARGET ... POST_BUILD)
# rather than a separate ALL custom_target/custom_command pair. The latter
# pattern caused an infinite reconfigure loop because cpack writes manifest
# files under the build directory which the build system then sees as
# invalidating downstream targets, re-runs cmake, re-runs cpack, etc.
# POST_BUILD fires exactly once per link of the named target — no stamps,
# no file-level DEPENDS on source files, no reconfigure churn.
option(QIFTOP_AUTO_PACKAGE
    "Run cpack -G DEB automatically after each build" ON)
if(QIFTOP_AUTO_PACKAGE)
    add_custom_command(TARGET qiftop-agent POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "[qiftop] regenerating .deb packages"
        COMMAND ${CMAKE_CPACK_COMMAND} -G DEB
                --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Repackaging qiftop / qiftop-agent .deb"
        VERBATIM)
endif()

# --- Tests ------------------------------------------------------------------
# Unit tests for pure-logic helpers (heuristics, EMA, settings migration).
# Disable with -DQIFTOP_BUILD_TESTS=OFF for distro builds.
option(QIFTOP_BUILD_TESTS "Build unit tests" ON)
if(QIFTOP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
