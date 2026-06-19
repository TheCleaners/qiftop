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
    ProcDetails.cpp
    BpfBirthReader.cpp
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

# --- eBPF socket-birth attribution program (gated on QIFTOP_HAVE_BPF) --------
# Compile the CO-RE program (clang -target bpf) and generate a libbpf skeleton
# header that EMBEDS the object bytes — so nothing extra ships in the package;
# the program rides inside the agent binary. QIFTOP_HAVE_BPF is resolved by the
# skip-safe toolchain detection in the top-level CMakeLists (clang+bpftool+
# libbpf all present); when off, none of this runs and the agent is
# conntrack-only. See AGENTS.md §4 / docs/HACKING.md.
if(QIFTOP_HAVE_BPF)
    # bpf_tracing.h keys its (unused-for-fentry) register macros off the target
    # arch; pass the right token so the header is happy on non-x86 builders too.
    set(_bpf_arch "${CMAKE_SYSTEM_PROCESSOR}")
    if(_bpf_arch MATCHES "x86_64|amd64|i.86")
        set(_bpf_arch x86)
    elseif(_bpf_arch MATCHES "aarch64|arm64")
        set(_bpf_arch arm64)
    elseif(_bpf_arch MATCHES "^arm")
        set(_bpf_arch arm)
    elseif(_bpf_arch MATCHES "ppc64|powerpc")
        set(_bpf_arch powerpc)
    elseif(_bpf_arch MATCHES "s390")
        set(_bpf_arch s390)
    elseif(_bpf_arch MATCHES "riscv")
        set(_bpf_arch riscv)
    elseif(_bpf_arch MATCHES "loongarch")
        set(_bpf_arch loongarch)
    endif()

    set(_bpf_src  ${CMAKE_CURRENT_SOURCE_DIR}/bpf/birth.bpf.c)
    set(_bpf_obj  ${CMAKE_CURRENT_BINARY_DIR}/birth.bpf.o)
    set(_bpf_skel ${CMAKE_CURRENT_BINARY_DIR}/birth.skel.h)

    # -g is REQUIRED: bpftool gen skeleton needs the .BTF the debug build emits.
    set(_bpf_cflags -g -O2 -target bpf -D__TARGET_ARCH_${_bpf_arch}
        -Wall -Werror -I${CMAKE_CURRENT_SOURCE_DIR}/bpf)
    foreach(_d ${LIBBPF_INCLUDE_DIRS})
        list(APPEND _bpf_cflags -I${_d})
    endforeach()

    add_custom_command(
        OUTPUT  ${_bpf_obj}
        COMMAND ${QIFTOP_CLANG_BPF} ${_bpf_cflags} -c ${_bpf_src} -o ${_bpf_obj}
        DEPENDS ${_bpf_src}
                ${CMAKE_CURRENT_SOURCE_DIR}/bpf/vmlinux_min.h
                ${CMAKE_CURRENT_SOURCE_DIR}/bpf/birth_events.h
        COMMENT "BPF   birth.bpf.c -> birth.bpf.o (${_bpf_arch})"
        VERBATIM)

    add_custom_command(
        OUTPUT  ${_bpf_skel}
        COMMAND ${QIFTOP_BPFTOOL} gen skeleton ${_bpf_obj} name qiftop_birth > ${_bpf_skel}
        DEPENDS ${_bpf_obj}
        COMMENT "BPF   gen skeleton -> birth.skel.h"
        VERBATIM)

    add_custom_target(qiftop_bpf_skel DEPENDS ${_bpf_skel})

    # The userspace loader/reader (next PR) lives in backend_linux and includes
    # "birth.skel.h"; build the skeleton before it and put it on the include
    # path now so the wiring is mechanical. Compiling the skeleton in CI also
    # validates the eBPF program on every push (where the toolchain is present).
    add_dependencies(backend_linux qiftop_bpf_skel)
    target_include_directories(backend_linux PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}    # birth.skel.h
        ${LIBBPF_INCLUDE_DIRS})
    target_link_libraries(backend_linux PRIVATE ${LIBBPF_LIBRARIES})
endif()

# backend/linux TUs gate the eBPF reader on QIFTOP_HAVE_BPF (and share the other
# attribution toggles), so hand this target the same compile definitions the
# rest of the tree gets.
qiftop_apply_attribution_defs(backend_linux)
