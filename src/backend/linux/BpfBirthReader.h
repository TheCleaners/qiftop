#pragma once
#ifdef QIFTOP_HAVE_BPF

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "backend/BirthCache.h"

// Forward-declare the libbpf + generated-skeleton types so this header stays
// free of libbpf / birth.skel.h (those live only in the .cpp).
struct ring_buffer;
struct qiftop_birth;
struct bpf_link;

namespace qiftop::backend::linuximpl {

// Loads the CO-RE socket-birth eBPF program, attaches its fexit/fentry probes,
// and drains the ring buffer on a dedicated thread, handing each decoded birth
// to a sink (in production: BpfBirthResolver::onBirth). One reader per agent.
//
// Skip-safe by construction: start() returns false — and the reader stays inert
// with nothing attached — on any kernel that lacks BTF, BPF trampolines
// (CONFIG_FUNCTION_TRACER), or the CAP_BPF/CAP_PERFMON the load needs. The
// caller then simply doesn't setLoaded() the resolver and the chain runs
// conntrack-only. Only compiled when QIFTOP_HAVE_BPF (the skeleton exists).
class BpfBirthReader {
public:
    using Sink = std::function<void(const BirthKey &, const BirthRecord &)>;

    explicit BpfBirthReader(Sink sink);
    ~BpfBirthReader();

    BpfBirthReader(const BpfBirthReader &)            = delete;
    BpfBirthReader &operator=(const BpfBirthReader &) = delete;

    // Open + load + attach the program and spawn the drain thread. Returns
    // false (inert) on any failure; safe to call once. Idempotent if already
    // running (returns true).
    [[nodiscard]] bool start();

    // Stop the drain thread and tear down the program. Safe to call multiple
    // times and from the destructor.
    void stop();

    [[nodiscard]] bool running() const { return m_running.load(); }

private:
    static int onRingEvent(void *ctx, void *data, unsigned long size);
    void       drainLoop();

    Sink              m_sink;
    qiftop_birth     *m_skel = nullptr;
    ring_buffer      *m_rb   = nullptr;
    std::vector<bpf_link *> m_links; // per-probe links we attached + own
    std::thread       m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_running{false};
    long              m_clkTck = 0;
};

} // namespace qiftop::backend::linuximpl

#endif // QIFTOP_HAVE_BPF
