# Shared BSD-family platform backend (NetBSD, FreeBSD, OpenBSD, DragonFly,
# and largely macOS). Per-interface counters come from getifaddrs(3) AF_LINK
# records, which are identical across every BSD. Per-flow accounting uses
# libpcap/BPF + a userspace flow table (the portable datapath — there is no
# conntrack); the parsing/accounting logic is platform-neutral and a
# candidate to backport to Linux as an alternative capture path. Linked into
# libqiftop on BSD targets, mirroring how backend_linux is absorbed on Linux.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PCAP IMPORTED_TARGET libpcap)
endif()
if(NOT PCAP_FOUND)
    # Base-system libpcap (NetBSD/FreeBSD/OpenBSD/macOS) without a .pc file.
    find_path(PCAP_INCLUDE_DIR pcap.h)
    find_library(PCAP_LIBRARY NAMES pcap)
    if(NOT PCAP_LIBRARY)
        message(FATAL_ERROR "libpcap not found (needed by backend_bsd for per-flow capture)")
    endif()
endif()

add_library(backend_bsd STATIC
    BsdNetworkWorker.cpp
    BsdNetworkMonitor.cpp
    BsdConnectionWorker.cpp
    BsdConnectionMonitor.cpp
)

target_include_directories(backend_bsd
    PUBLIC ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(backend_bsd
    PUBLIC Qt6::Core Qt6::Network
)

if(PCAP_FOUND)
    target_link_libraries(backend_bsd PRIVATE PkgConfig::PCAP)
else()
    target_include_directories(backend_bsd PRIVATE ${PCAP_INCLUDE_DIR})
    target_link_libraries(backend_bsd PRIVATE ${PCAP_LIBRARY})
endif()
