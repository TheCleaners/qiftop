# Shared BSD-family platform backend (NetBSD, FreeBSD, OpenBSD, DragonFly,
# and largely macOS). Pure POSIX + Qt — no external libraries. Per-interface
# counters come from getifaddrs(3) AF_LINK records, which are identical across
# every BSD; per-flow capture is a stub pending a pf/npf/BPF datapath (the one
# place the BSDs diverge — OpenBSD/FreeBSD use pf, NetBSD uses npf). Linked
# into libqiftop on BSD targets, mirroring how backend_linux is absorbed on
# Linux.
add_library(backend_bsd STATIC
    BsdNetworkWorker.cpp
    BsdNetworkMonitor.cpp
    BsdConnectionMonitor.cpp
)

target_include_directories(backend_bsd
    PUBLIC ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(backend_bsd
    PUBLIC Qt6::Core Qt6::Network
)
