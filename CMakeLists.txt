cmake_minimum_required(VERSION 3.21)
project(qiftop VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# libqiftop is a SHARED library that absorbs the static backend_<platform>
# lib, so every object that can end up inside it must be position-independent.
# (Executables become PIE, which is already the toolchain default.)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
# --- Platform backend (static; absorbed into libqiftop below) ---------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(src/backend/linux)
else()
    message(FATAL_ERROR "No backend available for platform: ${CMAKE_SYSTEM_NAME}")
endif()

# --- libqiftop: shared, Widgets-free core -----------------------------------
# Qt6::Core + Network + DBus ONLY (NO Widgets). Every frontend links this: the
# GUI (qiftop), the ncurses TUI (nqiftop), the privileged agent, and any
# future exporter / alerting consumer. Anything pulling in QWidget /
# QAbstractItemView / a delegate belongs in qiftop_ui, NOT here. The static
# platform backend (backend_linux) is absorbed PUBLIC so consumers get the
# in-process capture path + the resolver factory's #ifdef'd impls.
#
# NOTE: the long-term goal is to further split a Qt6::Core-only `qiftop_core`
# seam (DTOs, filter mini-language, unit formatters, aggregation/EMA) out of
# this into a libqiftop-core package. Keep new pure-logic TUs free of
# Network/DBus includes so that carve-out stays mechanical.
add_library(qiftoplib SHARED
    src/dbus/Types.cpp
    src/backend/NetworkMonitor.cpp
    src/backend/ConnectionMonitor.cpp
    src/backend/PlatformInfo.cpp
    src/backend/ProcessResolverFactory.cpp
    src/backend/dbus/DBusNetworkMonitor.cpp
    src/backend/dbus/DBusConnectionMonitor.cpp
    src/config/Settings.cpp
    src/dns/DnsResolver.cpp
    src/dns/QtDnsResolver.cpp
    src/util/ConnectionFilter.cpp
    src/util/Exporter.cpp
    src/util/Logging.cpp
    src/util/Autostart.cpp
    src/aggregate/InterfaceAggregator.cpp
    src/aggregate/ConnectionAggregator.cpp
)
set_target_properties(qiftoplib PROPERTIES
    OUTPUT_NAME qiftop
    VERSION   ${PROJECT_VERSION}
    SOVERSION 0)
target_include_directories(qiftoplib PUBLIC src)
target_link_libraries(qiftoplib PUBLIC Qt6::Core Qt6::Network Qt6::DBus)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(qiftoplib PUBLIC backend_linux)
    target_compile_definitions(qiftoplib PUBLIC BACKEND_LINUX)
endif()
qiftop_apply_attribution_defs(qiftoplib)

# --- Client GUI layer (qiftop_ui, static, Widgets) --------------------------
# Widgets-only: MainWindow, dialogs, delegates, the QAbstractItemModel
# adapters + proxies, tray, and the GUI-only elevation path. Links libqiftop.
# Both the qiftop executable and the widget-level smoke tests
# (tests/test_mainwindow_smoke.cpp) link this so the tests exercise the real
# production widget tree, models, and proxies — not stand-in lookalikes.
add_library(qiftop_ui STATIC
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
    src/util/HandoffClient.cpp
    src/util/HandoffServer.cpp
    src/util/PrivilegeEscalator.cpp
)
target_include_directories(qiftop_ui PUBLIC src)
target_link_libraries(qiftop_ui PUBLIC
    Qt6::Core Qt6::Widgets Qt6::Network Qt6::DBus qiftoplib)
qiftop_apply_attribution_defs(qiftop_ui)

add_executable(qiftop src/main.cpp)
target_link_libraries(qiftop PRIVATE qiftop_ui)
# Single source of truth for the release version: derive the runtime
# --version string from the CMake project() version instead of hardcoding it.
target_compile_definitions(qiftop PRIVATE QIFTOP_VERSION="${PROJECT_VERSION}")

# --- Privileged agent (qiftop-agent) ----------------------------------------
# Agent-specific TUs only; everything shared (monitors, resolver factory,
# backend, DTO marshalling, logging) comes in via libqiftop.
add_executable(qiftop-agent
    src/agent/main.cpp
    src/agent/Application.cpp
    src/agent/Config.cpp
    src/agent/InterfacesService.cpp
    src/agent/ConnectionsService.cpp
    src/agent/Attribution.cpp
    src/agent/IdleManager.cpp
)
target_include_directories(qiftop-agent PRIVATE src)
target_link_libraries(qiftop-agent PRIVATE Qt6::Core Qt6::DBus Qt6::Network qiftoplib)
target_compile_definitions(qiftop-agent PRIVATE QIFTOP_VERSION="${PROJECT_VERSION}")
qiftop_apply_attribution_defs(qiftop-agent)

# --- ncurses TUI (nqiftop) --------------------------------------------------
# Headless frontend built entirely on libqiftop (no Qt Widgets). Needs
# ncursesw (wide chars for UTF-8 arrows) and, like the GUI, the platform
# backend for the in-process fallback when no agent is reachable.
option(QIFTOP_BUILD_TUI "Build the ncurses nqiftop frontend" ON)
if(QIFTOP_BUILD_TUI AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CURSES_NEED_WIDE TRUE)
    find_package(Curses)
    if(CURSES_FOUND)
        add_executable(nqiftop
            src/tui/main.cpp
            src/tui/TuiApp.cpp
            src/tui/Screen.cpp)
        target_include_directories(nqiftop PRIVATE src ${CURSES_INCLUDE_DIRS})
        target_link_libraries(nqiftop PRIVATE qiftoplib backend_linux ${CURSES_LIBRARIES})
        target_compile_definitions(nqiftop PRIVATE
            BACKEND_LINUX QIFTOP_VERSION="${PROJECT_VERSION}")
        qiftop_apply_attribution_defs(nqiftop)
    else()
        message(WARNING "ncursesw not found — nqiftop will not be built "
                        "(install libncurses-dev / ncurses-devel)")
    endif()
endif()

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

    # Terminal (ncurses) demo harness — only when the TUI itself is built.
    if(TARGET nqiftop)
        add_executable(nqiftop-demo
            docs/demo/nqiftop-demo.cpp
            src/tui/Screen.cpp
            src/tui/TuiApp.cpp
            tests/fakes/FakeMonitors.h)
        target_link_libraries(nqiftop-demo PRIVATE qiftoplib ${CURSES_LIBRARIES})
        target_include_directories(nqiftop-demo PRIVATE
            ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests ${CURSES_INCLUDE_DIRS})
        qiftop_apply_attribution_defs(nqiftop-demo)
    endif()
endif()

# --- Install rules ----------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# --- libqiftop: runtime + development packaging -----------------------------
# The shared core library is split across two install components:
#   libqiftop      — the runtime .so + SONAME symlink (libqiftop.so.0[.x.y])
#   libqiftop-dev  — the dev symlink (libqiftop.so), public headers, the
#                    CMake package config, and the pkg-config file.
# This is what makes libqiftop a reusable "batch/stream raw-data facility":
# external consumers (a Prometheus exporter, an alerting daemon, language
# bindings, an NDJSON CLI) link the runtime and build against -dev. They get
# the DBus client proxies (DBus*Monitor) to stream from the agent, the
# aggregators for the smoothed/derived view, the filter mini-language, the
# DTOs, and the JSON/CSV exporter — all Widgets-free.
install(TARGETS qiftoplib
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            COMPONENT libqiftop
            NAMELINK_COMPONENT libqiftop-dev)

# Public headers — the Widgets-free API surface. Installed under
# include/qiftop/ preserving the source subdirectory layout (the dev CMake
# config / pkg-config add include/qiftop to the search path so the headers'
# own "backend/..."-style cross-includes resolve). The GUI (ui/) and the
# agent are not shipped, and the platform capture impls (backend/linux) are
# excluded — consumers stream from the agent over DBus, they don't link the
# privileged in-process path.
foreach(_hdr_dir aggregate dbus config dns util)
    install(DIRECTORY src/${_hdr_dir}/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qiftop/${_hdr_dir}
            COMPONENT libqiftop-dev
            FILES_MATCHING PATTERN "*.h")
endforeach()
# backend/ recursively (dbus client proxies + abstract ifaces + null), but
# NOT backend/linux (the libnl/conntrack capture impl).
install(DIRECTORY src/backend/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qiftop/backend
        COMPONENT libqiftop-dev
        FILES_MATCHING PATTERN "*.h"
        PATTERN "linux" EXCLUDE)

# CMake package config (find_package(qiftop) -> qiftop::qiftop).
configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/dist/cmake/qiftopConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/qiftopConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/qiftop"
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/qiftopConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    # Pre-1.0 the ABI is unstable; require an exact version match so a
    # consumer built against 0.2.1 doesn't silently load a different one.
    COMPATIBILITY ExactVersion)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/qiftopConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/qiftopConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/qiftop"
        COMPONENT libqiftop-dev)

# pkg-config (for non-CMake / C / Rust / scripting consumers).
# Compute a relocatable prefix: each path segment of <libdir>/pkgconfig
# becomes a ".." so the .pc's prefix is derived from ${pcfiledir} at use
# time (correct under /usr/local dev installs AND /usr CPack packages).
string(REGEX REPLACE "[^/]+" ".." QIFTOP_PC_PREFIX_UP "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
set(QIFTOP_PC_PREFIX "\${pcfiledir}/${QIFTOP_PC_PREFIX_UP}")
configure_file(
    "${CMAKE_SOURCE_DIR}/dist/pkgconfig/qiftop.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/qiftop.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qiftop.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        COMPONENT libqiftop-dev)

install(TARGETS qiftop
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT qiftop)

install(TARGETS qiftop-agent
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT qiftop-agent)

# nqiftop is conditional (QIFTOP_BUILD_TUI + ncursesw present).
if(TARGET nqiftop)
    install(TARGETS nqiftop
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT nqiftop)
endif()

# Man pages. Debian policy + lintian require installed man pages to be
# gzip-compressed (`uncompressed-manual-page` is a lintian error). CPack
# does not run debhelper's dh_compress, so we gzip them at build time and
# install the .gz ourselves. `gzip -n` drops the name/mtime header for
# reproducible packages. Grouped by component so each binary ships its
# own page(s).
find_program(GZIP_TOOL gzip)
function(qiftop_install_manpage SRC SECTION COMPONENT)
    if(GZIP_TOOL)
        get_filename_component(_name "${SRC}" NAME)
        set(_gz "${CMAKE_BINARY_DIR}/man/${_name}.gz")
        add_custom_command(
            OUTPUT  "${_gz}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/man"
            COMMAND ${GZIP_TOOL} -9 -n -c "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}" > "${_gz}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
            COMMENT "Compressing man page ${_name}"
            VERBATIM)
        string(MAKE_C_IDENTIFIER "manpage_${_name}" _tgt)
        add_custom_target("${_tgt}" ALL DEPENDS "${_gz}")
        install(FILES "${_gz}"
                DESTINATION "${CMAKE_INSTALL_MANDIR}/man${SECTION}"
                COMPONENT "${COMPONENT}")
    else()
        # No gzip on the build host: install uncompressed rather than fail.
        message(WARNING "gzip not found; installing uncompressed man page ${SRC}")
        install(FILES "${SRC}"
                DESTINATION "${CMAKE_INSTALL_MANDIR}/man${SECTION}"
                COMPONENT "${COMPONENT}")
    endif()
endfunction()

qiftop_install_manpage(dist/man/qiftop.1            1 qiftop)
qiftop_install_manpage(dist/man/qiftop-agent.8      8 qiftop-agent)
qiftop_install_manpage(dist/man/qiftop-agent.conf.5 5 qiftop-agent)
if(TARGET nqiftop)
    qiftop_install_manpage(dist/man/nqiftop.1       1 nqiftop)
endif()

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
set(CPACK_PACKAGE_HOMEPAGE_URL      "https://github.com/TheCleaners/qiftop")
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
# Defaults to the project() version. Overridable: pre-release tags
# (release.yml) pass e.g. -DCPACK_PACKAGE_VERSION=0.2~rc1 so the .deb
# version + filename carry the full tag. The Debian "~" makes a prerelease
# sort BEFORE the final (0.2~rc1 < 0.2), so apt upgrades rc → final cleanly;
# a literal "-rc1" would sort NEWER than 0.2 and block the upgrade.
if(NOT DEFINED CPACK_PACKAGE_VERSION)
    set(CPACK_PACKAGE_VERSION       "${PROJECT_VERSION}")
endif()

set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)
# One file per component, named e.g. qiftop_0.2_amd64.deb (or, for a
# pre-release tag, qiftop_0.2~rc1_amd64.deb).
set(CPACK_DEBIAN_FILE_NAME          "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS  ON)
# Generate a Debian `shlibs` control file for packages that ship a shared
# library (libqiftop0). Without it lintian errors with `no-shlibs`, and
# downstream packages linking libqiftop can't auto-derive their dependency.
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
# qiftop / qiftop-agent / nqiftop link our own libqiftop.so.0, which lives in
# a SEPARATE component (libqiftop) — so dpkg-shlibdeps can't find it on the
# system or in the per-component staging dir and aborts with "cannot find
# library libqiftop.so.0". Point it at the build tree (where the .so + SONAME
# symlink are) via -l so it resolves; the resulting "no shlibs info" is a
# harmless warning (the explicit `libqiftop0 (= version)` Depends already
# pins the dependency). Without this, `cpack -G DEB` fails outright.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${CMAKE_BINARY_DIR}")
set(CPACK_DEBIAN_PACKAGE_SECTION    "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")

# --- GUI package ------------------------------------------------------------
set(CPACK_DEBIAN_QIFTOP_PACKAGE_NAME         "qiftop")
set(CPACK_DEBIAN_QIFTOP_PACKAGE_DEPENDS      "libc6, libqt6core6t64 | libqt6core6, libqt6widgets6, libqt6network6, libqt6dbus6, libqiftop0 (= ${CPACK_PACKAGE_VERSION})")
# Pin to the exact package version built in this run (incl. any ~rcN suffix)
# so the GUI Recommends matches the agent .deb produced alongside it.
set(CPACK_DEBIAN_QIFTOP_PACKAGE_RECOMMENDS   "qiftop-agent (= ${CPACK_PACKAGE_VERSION})")
set(CPACK_DEBIAN_QIFTOP_DESCRIPTION
"Qt6 iftop-style network monitor (GUI).
qiftop is a Qt 6 desktop application that displays live per-interface
traffic counters and per-connection flows. It cooperates with the
qiftop-agent system service for privileged data access but can also
fall back to running with elevated privileges on demand.")

# --- Agent package ----------------------------------------------------------
set(CPACK_DEBIAN_QIFTOP-AGENT_PACKAGE_NAME       "qiftop-agent")
set(CPACK_DEBIAN_QIFTOP-AGENT_PACKAGE_DEPENDS    "libc6, libnl-3-200, libnl-route-3-200, libnetfilter-conntrack3, libqt6core6t64 | libqt6core6, libqt6network6, libqt6dbus6, dbus, libqiftop0 (= ${CPACK_PACKAGE_VERSION})")
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

# --- libqiftop runtime + dev packages ---------------------------------------
# Debian shared-library convention: lib<name><soversion> for the runtime
# (libqiftop0) and lib<name>-dev for the headers.
set(CPACK_DEBIAN_LIBQIFTOP_PACKAGE_NAME    "libqiftop0")
set(CPACK_DEBIAN_LIBQIFTOP_PACKAGE_SECTION "libs")
# Shared-library package must run ldconfig on install/removal. Ship a dpkg
# `triggers` file (`activate-noawait ldconfig`) so dpkg fires libc's ldconfig
# trigger — the modern equivalent of a postinst `ldconfig` call, and what
# lintian's `lacks-ldconfig-trigger` check expects.
set(CPACK_DEBIAN_LIBQIFTOP_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/dist/debian/triggers")
set(CPACK_DEBIAN_LIBQIFTOP_DESCRIPTION
"Qt6 network-monitoring data library (runtime).
libqiftop carries the qiftop wire DTOs, the DBus client proxies for the
qiftop-agent, the per-interface / per-connection aggregators, the filter
mini-language, IEC unit formatters, and JSON/CSV export — everything a
non-GUI consumer (metrics exporter, alerting daemon, language bindings,
NDJSON CLI) needs to stream and process qiftop data. No Qt Widgets
dependency, so it runs headless.")

set(CPACK_DEBIAN_LIBQIFTOP-DEV_PACKAGE_NAME    "libqiftop-dev")
set(CPACK_DEBIAN_LIBQIFTOP-DEV_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_LIBQIFTOP-DEV_PACKAGE_DEPENDS "libqiftop0 (= ${CPACK_PACKAGE_VERSION}), qt6-base-dev")
set(CPACK_DEBIAN_LIBQIFTOP-DEV_DESCRIPTION
"Qt6 network-monitoring data library (development files).
Public headers, the CMake package config (find_package(qiftop) ->
qiftop::qiftop), and the pkg-config file for building against libqiftop.")

# --- nqiftop (ncurses frontend) package -------------------------------------
set(CPACK_DEBIAN_NQIFTOP_PACKAGE_NAME    "nqiftop")
set(CPACK_DEBIAN_NQIFTOP_PACKAGE_SECTION "net")
# Library deps (Qt6, ncursesw) are auto-resolved by dpkg-shlibdeps; libqiftop0
# is our own lib, so declare it explicitly + pinned to this build.
set(CPACK_DEBIAN_NQIFTOP_PACKAGE_DEPENDS "libqiftop0 (= ${CPACK_PACKAGE_VERSION})")
set(CPACK_DEBIAN_NQIFTOP_PACKAGE_RECOMMENDS "qiftop-agent")
set(CPACK_DEBIAN_NQIFTOP_DESCRIPTION
"ncurses network monitor (qiftop, headless).
nqiftop is a terminal UI for live per-interface and per-connection traffic,
built on libqiftop. It streams from the qiftop-agent over DBus, or captures
in-process when run as root — no X/Wayland needed, so it works over SSH.")

# --- RPM generator (Fedora / openSUSE etc.) ---------------------------------
# Mirrors the DEB config above. Built with `cpack -G RPM` (needs rpmbuild +
# the Fedora -devel libs). Library Requires are discovered automatically by
# rpmbuild's find-requires (CPACK_RPM_PACKAGE_AUTOREQ defaults ON), so we only
# declare the non-library deps. Filenames follow RPM convention:
# name-version-release.dist.arch.rpm, e.g. qiftop-0.2~rc1-1.fc44.x86_64.rpm
# (rpm 4.10+ sorts the '~' before the final release, matching the DEB '~rcN').
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_RPM_FILE_NAME            "RPM-DEFAULT")
set(CPACK_RPM_PACKAGE_LICENSE      "GPLv2+")
set(CPACK_RPM_PACKAGE_GROUP        "Applications/System")
set(CPACK_RPM_PACKAGE_URL          "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_VENDOR       "${CPACK_PACKAGE_VENDOR}")
# Append the %{?dist} tag (.fc44 etc.) — standard Fedora practice.
set(CPACK_RPM_PACKAGE_RELEASE      "1")
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)

# GUI subpackage
set(CPACK_RPM_QIFTOP_PACKAGE_NAME     "qiftop")
set(CPACK_RPM_QIFTOP_PACKAGE_SUMMARY  "Qt6 iftop-style network monitor (GUI)")
# Weak dep on the agent (rpm Recommends), matching the DEB Recommends. Pinned
# to the exact version built alongside it.
set(CPACK_RPM_QIFTOP_PACKAGE_RECOMMENDS "qiftop-agent = ${CPACK_PACKAGE_VERSION}")

# Agent subpackage
set(CPACK_RPM_QIFTOP-AGENT_PACKAGE_NAME    "qiftop-agent")
set(CPACK_RPM_QIFTOP-AGENT_PACKAGE_SUMMARY "Privileged DBus agent for qiftop")
set(CPACK_RPM_QIFTOP-AGENT_PACKAGE_REQUIRES   "dbus, shadow-utils")
set(CPACK_RPM_QIFTOP-AGENT_PACKAGE_RECOMMENDS "nftables")

# libqiftop runtime + dev subpackages. RPM auto-req resolves the SONAME
# (qiftop-libs Provides libqiftop.so.0()(64bit); qiftop/qiftop-agent
# auto-Require it via their ELF NEEDED), so no explicit lib dep is declared.
set(CPACK_RPM_LIBQIFTOP_PACKAGE_NAME    "qiftop-libs")
set(CPACK_RPM_LIBQIFTOP_PACKAGE_SUMMARY "Qt6 network-monitoring data library (runtime)")
set(CPACK_RPM_LIBQIFTOP-DEV_PACKAGE_NAME     "qiftop-devel")
set(CPACK_RPM_LIBQIFTOP-DEV_PACKAGE_SUMMARY  "Development files for qiftop-libs")
set(CPACK_RPM_LIBQIFTOP-DEV_PACKAGE_REQUIRES "qiftop-libs = ${CPACK_PACKAGE_VERSION}, qt6-qtbase-devel")

# nqiftop rpm: auto-req resolves libqiftop.so.0 + ncursesw + Qt6.
set(CPACK_RPM_NQIFTOP_PACKAGE_NAME    "nqiftop")
set(CPACK_RPM_NQIFTOP_PACKAGE_SUMMARY "ncurses network monitor (qiftop, headless)")
set(CPACK_RPM_NQIFTOP_PACKAGE_RECOMMENDS "qiftop-agent")
# Preserve user edits to the config file across upgrades (Fedora %config(noreplace)).
set(CPACK_RPM_QIFTOP-AGENT_USER_FILELIST "%config(noreplace) /etc/qiftop/agent.conf")
# Group creation + user enrollment + systemd reload, mirroring the DEB
# maintainer scripts (RPM scriptlet semantics differ — see the files).
set(CPACK_RPM_QIFTOP-AGENT_POST_INSTALL_SCRIPT_FILE
    "${CMAKE_SOURCE_DIR}/dist/rpm/post-agent.sh")
set(CPACK_RPM_QIFTOP-AGENT_POST_UNINSTALL_SCRIPT_FILE
    "${CMAKE_SOURCE_DIR}/dist/rpm/postun-agent.sh")
# Don't claim ownership of standard system directories that are already owned
# by the `filesystem`, `systemd`, `dbus`, and `hicolor-icon-theme` packages —
# otherwise rpm reports a file conflict (notably `/lib`, which is a usrmerge
# symlink owned by `filesystem`). The package-specific dirs (/etc/qiftop,
# /usr/share/qiftop) are deliberately NOT excluded so we keep owning them.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/lib;/lib/systemd;/lib/systemd/system;/usr/share/dbus-1;/usr/share/dbus-1/system.d;/usr/share/dbus-1/system-services;/usr/share/applications;/usr/share/icons;/usr/share/icons/hicolor;/usr/share/icons/hicolor/scalable;/usr/share/icons/hicolor/scalable/apps;/usr/lib/cmake;/usr/lib/pkgconfig;/usr/lib64/cmake;/usr/lib64/pkgconfig")

# Pre-release RPMs keep inline debug info (unstripped), mirroring the
# unstripped pre-release .deb so user-reported crash backtraces are
# debuggable. CPACK_STRIP_FILES=OFF is the release workflow's signal for a
# "debuggable build" (set for pre-release tags); for it we neutralise BOTH
# of rpmbuild's symbol-shedding passes that would otherwise discard the
# DWARF that CPACK_STRIP_FILES=OFF preserved at install time:
#   * `debug_package %{nil}`  — don't split symbols into a -debuginfo
#     subpackage (the .deb keeps them inline, so the .rpm does too).
#   * `__strip /bin/true`     — neutralise brp-strip so the binaries in the
#     payload stay unstripped with their .debug_* sections intact.
# Final tags strip normally (CPACK_STRIP_FILES defaults TRUE above).
if(NOT CPACK_STRIP_FILES)
    set(CPACK_RPM_DEBUGINFO_PACKAGE OFF)
    set(CPACK_RPM_SPEC_MORE_DEFINE
"%global debug_package %{nil}
%global __strip /bin/true")
endif()

include(CPack)
cpack_add_component(qiftop
    DISPLAY_NAME "qiftop GUI"
    DESCRIPTION  "Qt6 iftop-style network monitor (GUI)")
cpack_add_component(qiftop-agent
    DISPLAY_NAME "qiftop privileged agent"
    DESCRIPTION  "DBus system-bus daemon exposing privileged network statistics")
cpack_add_component(libqiftop
    DISPLAY_NAME "libqiftop runtime"
    DESCRIPTION  "Shared Qt6 data library (DTOs, DBus client, aggregators, filter, export)")
cpack_add_component(libqiftop-dev
    DISPLAY_NAME "libqiftop development files"
    DESCRIPTION  "Headers, CMake config and pkg-config for building against libqiftop")
if(TARGET nqiftop)
    cpack_add_component(nqiftop
        DISPLAY_NAME "nqiftop ncurses frontend"
        DESCRIPTION  "Headless terminal UI for live interface + connection traffic")
endif()

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
