// SPDX-License-Identifier: GPL-2.0
/*
 * qiftop eBPF socket-BIRTH attribution program (v0.4 birth+conntrack hybrid).
 *
 * Phase-0 measurement found conntrack capture is fine, but short-lived
 * processes are ~100% unattributed: the owning process has EXITED by the time
 * the ~1 s snapshot + sock_diag walk runs. This program captures the owning
 * pid + direction + 5-tuple at flow BIRTH — the instant connect()/accept()/
 * first connected UDP send fires, synchronously in the owner's context, before
 * it can die — and pushes it to userspace over a ring buffer. The userspace
 * BpfBirthResolver looks each conntrack flow up here FIRST, recovering the pid
 * sock_diag would miss. The hybrid still gets BYTES from conntrack; birth has
 * none.
 *
 * This is the CO-RE production sibling of bench/integration/bpf-eval/birth.bt.
 * It mirrors that validated probe set, but uses BPF trampolines (fentry/fexit)
 * instead of k(ret)probes:
 *   - fexit  tcp_v{4,6}_connect → outbound TCP (port assigned by return)
 *   - fentry udp_sendmsg        → outbound UDP (connected sends only)
 *   - fexit  inet_csk_accept    → inbound  TCP (return is the child sock)
 *
 * Why fexit and not the kretprobe the bench used: fexit reads args AND the
 * return value directly from the trampoline (no pt_regs, no arch-specific
 * register macros, no entry→return sock-stash map) and is cheaper than a
 * kretprobe. Semantically identical: at tcp_v*_connect RETURN the ephemeral
 * source port is assigned and we're still in the caller's process context —
 * the same reason the bench used the kRETprobe, not the SYN_SENT tracepoint
 * (which fires before inet_hash_connect() assigns the port, reporting
 * local_port=0).
 *
 * Portability: CO-RE relocates every struct-field access against the target
 * kernel's BTF, so this one object loads unchanged across kernels (validated
 * target range 6.6–7.1). It needs a BTF kernel (CONFIG_DEBUG_INFO_BTF=y) with
 * BPF trampolines (CONFIG_FUNCTION_TRACER=y + CONFIG_DEBUG_INFO_BTF), and the
 * ring buffer (>= 5.8) — all standard on distro kernels. The userspace loader
 * is skip-safe: if attach fails (no BTF / no trampoline support) the agent
 * runs conntrack-only.
 */
#include "vmlinux_min.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

#include "birth_events.h"

char LICENSE[] SEC("license") = "GPL";

#define AF_INET     2
#define AF_INET6   10
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

/* The births themselves. 1 MiB ring: at the measured ~3 µs/connect the kernel
 * side never blocks, and the userspace reader drains continuously. */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 20);
} births SEC(".maps");

/* Dedup connected-UDP births: udp_sendmsg fires per send, but a flow is born
 * once. Emit only the first send per sock pointer; LRU bounds it automatically
 * so a flood of distinct socks can never grow it unbounded. */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 8192);
    __type(key, __u64);
    __type(value, __u8);
} udp_seen SEC(".maps");

static __always_inline __u16 bswap16(__u16 v)
{
    return (__u16)((v >> 8) | (v << 8));
}

/* Fill pid/uid/comm/ts/start_time + the static proto+direction. */
static __always_inline void fill_meta(struct qiftop_birth_event *e,
                                      __u8 proto, __u8 dir)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();

    e->ts_ns     = bpf_ktime_get_ns();
    e->pid       = (__u32)(pid_tgid >> 32);
    e->uid       = (__u32)bpf_get_current_uid_gid();
    e->proto     = proto;
    e->direction = dir;
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    e->start_boottime_ns = BPF_CORE_READ(task, start_boottime);
}

/* Fill family + the 5-tuple from a `struct sock`. local = our bound end. */
static __always_inline void fill_tuple(struct qiftop_birth_event *e,
                                       struct sock *sk)
{
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);

    e->family      = (__u8)family;
    e->local_port  = BPF_CORE_READ(sk, __sk_common.skc_num);            /* host */
    e->remote_port = bswap16(BPF_CORE_READ(sk, __sk_common.skc_dport)); /* be→host */

    if (family == AF_INET6) {
        BPF_CORE_READ_INTO(&e->local_addr, sk,
                           __sk_common.skc_v6_rcv_saddr.in6_u.u6_addr8);
        BPF_CORE_READ_INTO(&e->remote_addr, sk,
                           __sk_common.skc_v6_daddr.in6_u.u6_addr8);
    } else {
        __be32 la = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
        __be32 ra = BPF_CORE_READ(sk, __sk_common.skc_daddr);
        __builtin_memcpy(e->local_addr, &la, sizeof(la));
        __builtin_memcpy(e->remote_addr, &ra, sizeof(ra));
    }
}

static __always_inline int emit(struct sock *sk, __u8 proto, __u8 dir)
{
    if (!sk)
        return 0;
    struct qiftop_birth_event *e =
        bpf_ringbuf_reserve(&births, sizeof(*e), 0);
    if (!e)
        return 0; /* ring full — drop; conntrack still covers the flow */
    __builtin_memset(e, 0, sizeof(*e));
    fill_meta(e, proto, dir);
    fill_tuple(e, sk);
    bpf_ringbuf_submit(e, 0);
    return 0;
}

/* --- outbound TCP: fexit, after the ephemeral port is assigned --------- */

/* int tcp_v4_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len) */
SEC("fexit/tcp_v4_connect")
int BPF_PROG(qiftop_tcp_v4_connect, struct sock *sk, void *uaddr, int addr_len,
             int ret)
{
    if (ret != 0)
        return 0; /* connect failed → no flow is born */
    return emit(sk, IPPROTO_TCP, QIFTOP_BIRTH_DIR_OUTBOUND);
}

/* int tcp_v6_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len) */
SEC("fexit/tcp_v6_connect")
int BPF_PROG(qiftop_tcp_v6_connect, struct sock *sk, void *uaddr, int addr_len,
             int ret)
{
    if (ret != 0)
        return 0;
    return emit(sk, IPPROTO_TCP, QIFTOP_BIRTH_DIR_OUTBOUND);
}

/* --- outbound UDP: connected sends only, deduped per sock -------------- */

/* int udp_sendmsg(struct sock *sk, struct msghdr *msg, size_t len) */
SEC("fentry/udp_sendmsg")
int BPF_PROG(qiftop_udp_sendmsg, struct sock *sk)
{
    /* Unconnected sends carry the destination in the msghdr, not the sock, and
     * leave skc_dport == 0 — skip them; we only attribute connected UDP. */
    __be16 dport = BPF_CORE_READ(sk, __sk_common.skc_dport);
    if (dport == 0)
        return 0;

    __u64 key = (__u64)(unsigned long)sk;
    if (bpf_map_lookup_elem(&udp_seen, &key))
        return 0; /* already emitted this flow's birth */
    __u8 one = 1;
    bpf_map_update_elem(&udp_seen, &key, &one, BPF_ANY);

    return emit(sk, IPPROTO_UDP, QIFTOP_BIRTH_DIR_OUTBOUND);
}

/* --- inbound TCP: the freshly-accepted child sock --------------------- */

/* struct sock *inet_csk_accept(struct sock *sk, int flags, int *err, bool kern) */
SEC("fexit/inet_csk_accept")
int BPF_PROG(qiftop_inet_csk_accept, struct sock *sk, int flags, void *err,
             int kern, struct sock *ret)
{
    /* ret is the freshly-accepted child sock; NULL on EAGAIN/interrupt. */
    return emit(ret, IPPROTO_TCP, QIFTOP_BIRTH_DIR_INBOUND);
}
