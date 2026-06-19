/*
 * Shared wire struct between the eBPF program (birth.bpf.c, kernel side) and
 * the userspace ring-buffer reader (backend/linux). Plain C, fixed-width, no
 * Qt — both sides must agree byte-for-byte.
 *
 * The fixed-width `__u*` types come from whatever the includer pulled in first:
 * vmlinux_min.h on the BPF side, <linux/types.h> on the userspace side. Keep
 * this header free of its own includes so it composes with both.
 */
#ifndef QIFTOP_BIRTH_EVENTS_H
#define QIFTOP_BIRTH_EVENTS_H

#define QIFTOP_BIRTH_COMM_LEN 16
#define QIFTOP_BIRTH_ADDR_LEN 16

/* Mirrors qiftop::backend::Direction (src/backend/Connection.h). Birth knows
 * the direction definitionally (connect = outbound, accept = inbound), unlike
 * conntrack which infers it heuristically. */
#define QIFTOP_BIRTH_DIR_UNKNOWN  0
#define QIFTOP_BIRTH_DIR_OUTBOUND 1
#define QIFTOP_BIRTH_DIR_INBOUND  2

/*
 * One captured socket birth, pushed through the ring buffer. Addresses are RAW
 * NETWORK-ORDER bytes (v4 occupies [0..4), v6 the full 16); ports are HOST
 * order. `start_boottime_ns` is task->start_boottime (the field /proc/<pid>/stat
 * field 22 is derived from) — the userspace reader converts it to clock ticks
 * for the PID-reuse guard (AGENTS.md §8a rule 2).
 *
 * Field order is size-descending to avoid implicit padding so the struct is
 * identical under the BPF and host C ABIs without packing pragmas.
 */
struct qiftop_birth_event {
    __u64 ts_ns;              /* bpf_ktime_get_ns() at birth (CLOCK_MONOTONIC)   */
    __u64 start_boottime_ns;  /* task->start_boottime — PID-reuse guard          */
    __u32 pid;                /* tgid (the userspace-visible PID)                */
    __u32 uid;                /* real uid of the owning task                     */
    __u16 local_port;         /* host byte order                                */
    __u16 remote_port;        /* host byte order                                */
    __u8  proto;              /* IPPROTO_TCP (6) / IPPROTO_UDP (17)             */
    __u8  family;             /* AF_INET (2) / AF_INET6 (10)                    */
    __u8  direction;          /* QIFTOP_BIRTH_DIR_*                             */
    __u8  _pad;
    __u8  local_addr[QIFTOP_BIRTH_ADDR_LEN];   /* network order, v4 in [0,4) */
    __u8  remote_addr[QIFTOP_BIRTH_ADDR_LEN];
    __u8  comm[QIFTOP_BIRTH_COMM_LEN];         /* kernel comm, NUL-padded    */
};

#endif /* QIFTOP_BIRTH_EVENTS_H */
