/*
 * Minimal CO-RE type subset for birth.bpf.c.
 *
 * Hand-written instead of a multi-megabyte generated vmlinux.h, on purpose: the
 * BUILD host then needs NO kernel BTF (CO-RE relocates these field accesses
 * against the TARGET kernel's BTF at load time — which is required regardless),
 * the header is arch-neutral, and it never drifts with the build box's kernel.
 *
 * Every record carries `preserve_access_index`, so each member access becomes a
 * CO-RE relocation: the offsets declared here are IRRELEVANT — libbpf rewrites
 * them per running kernel. We declare only the fields the program reads.
 * Anonymous-union kernel members (skc_daddr, skc_dport, …) are declared flat;
 * CO-RE resolves them by name against the target BTF, anonymous nesting and all.
 *
 * If this ever needs many more types, switch to a build-time
 * `bpftool btf dump file /sys/kernel/btf/vmlinux format c` with a checked-in,
 * dev-box-sanitised fallback. For this handful of fields, minimal wins.
 */
#ifndef QIFTOP_VMLINUX_MIN_H
#define QIFTOP_VMLINUX_MIN_H

typedef unsigned char      __u8;
typedef unsigned short     __u16;
typedef unsigned int       __u32;
typedef unsigned long long __u64;
typedef signed char        __s8;
typedef short              __s16;
typedef int                __s32;
typedef long long          __s64;
typedef __u16 __be16;
typedef __u32 __be32;
typedef __u32 __wsum;

/* UAPI enum bpf_map_type values we use (stable ABI; see <linux/bpf.h>). A full
 * generated vmlinux.h would supply these — declared minimally here so the build
 * host needs no kernel BTF. */
enum {
    BPF_MAP_TYPE_HASH     = 1,
    BPF_MAP_TYPE_LRU_HASH = 9,
    BPF_MAP_TYPE_RINGBUF  = 27,
};

/* bpf_map_update_elem() flags (UAPI). */
enum {
    BPF_ANY     = 0,
    BPF_NOEXIST = 1,
    BPF_EXIST   = 2,
};

#pragma clang attribute push(__attribute__((preserve_access_index)), apply_to = record)

struct in6_addr {
    union {
        __u8 u6_addr8[16];
    } in6_u;
};

struct sock_common {
    __be32 skc_daddr;
    __be32 skc_rcv_saddr;
    __be16 skc_dport;
    __u16  skc_num;
    unsigned short skc_family;
    struct in6_addr skc_v6_daddr;
    struct in6_addr skc_v6_rcv_saddr;
};

struct sock {
    struct sock_common __sk_common;
};

struct task_struct {
    __u64 start_time;
};

#pragma clang attribute pop

#endif /* QIFTOP_VMLINUX_MIN_H */
