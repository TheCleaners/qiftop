find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBNL REQUIRED libnl-3.0 libnl-route-3.0)
pkg_check_modules(LIBNFCT REQUIRED libnetfilter_conntrack)

add_library(backend_linux STATIC
    NetlinkWorker.cpp
    NetlinkMonitor.cpp
    ConntrackMonitor.cpp
    SockDiagDump.cpp
    SockDiagResolver.cpp
    CgroupClassifier.cpp
    NetnsScanner.cpp
)

target_include_directories(backend_linux
    PUBLIC  ${CMAKE_SOURCE_DIR}/src
    PRIVATE ${LIBNL_INCLUDE_DIRS} ${LIBNFCT_INCLUDE_DIRS}
)

target_link_libraries(backend_linux
    PUBLIC  Qt6::Core Qt6::Network
    PRIVATE ${LIBNL_LIBRARIES} ${LIBNFCT_LIBRARIES}
)

target_compile_options(backend_linux PRIVATE ${LIBNL_CFLAGS_OTHER} ${LIBNFCT_CFLAGS_OTHER})
