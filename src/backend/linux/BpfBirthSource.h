#pragma once
#ifdef QIFTOP_HAVE_BPF

#include <memory>

#include "backend/BpfBirthResolver.h"
#include "backend/linux/BpfBirthReader.h"
#include "backend/linux/ProcSnapshot.h"

// ProcessResolver adapter that owns the eBPF ring-buffer reader AND a
// BpfBirthResolver, wiring births from the former into the latter and gating
// the resolver's "loaded" state (and therefore its capabilities) on a
// successful program attach. This is the integration seam the factory adds
// FIRST in the chain so birth attribution wins for flows it saw.
//
// Lives in backend/linux because it couples the transport-neutral resolver
// core (backend/BpfBirthResolver.h) to the platform reader
// (backend/linux/BpfBirthReader.h). The composite only knows it as a
// ProcessResolver. Skip-safe: initialize() returns false (program couldn't
// load/attach — no BTF, no trampolines, no CAP_BPF) and the factory simply
// doesn't add it, leaving a clean conntrack-only chain.

namespace qiftop::backend::linuximpl {

class BpfBirthSource final : public ProcessResolver {
public:
    BpfBirthSource()
        : m_resolver(std::make_unique<BpfBirthResolver>(
              // PID-reuse guard probe: the live /proc field-22 starttime, in
              // the same clock-tick units BirthDecode stored at capture.
              [](qint32 pid) -> quint64 {
                  return procsnap::pidStartTime(pid).value_or(0);
              }))
    {
        // Feed each drained birth into the resolver. The reader thread owns the
        // call; BpfBirthResolver::onBirth is internally synchronised.
        m_reader = std::make_unique<BpfBirthReader>(
            [this](const BirthKey &k, const BirthRecord &r) {
                m_resolver->onBirth(k, r);
            });
    }

    ~BpfBirthSource() override
    {
        // Stop draining BEFORE the resolver (the sink captures it) is torn down.
        if (m_reader)
            m_reader->stop();
    }

    BpfBirthSource(const BpfBirthSource &)            = delete;
    BpfBirthSource &operator=(const BpfBirthSource &) = delete;

    // --- ProcessResolver ----------------------------------------------------
    bool initialize() override
    {
        if (!m_reader->start())
            return false;            // no BTF / trampolines / CAP_BPF
        m_resolver->setLoaded(true); // only NOW does it advertise caps / serve
        return true;
    }

    [[nodiscard]] QStringList capabilities() const override
    {
        return m_resolver->capabilities();
    }

    [[nodiscard]] qint32 resolvePid(const Connection &flow) override
    {
        return m_resolver->resolvePid(flow);
    }

    [[nodiscard]] std::optional<ProcessInfo> enrichPid(qint32 pid) override
    {
        return m_resolver->enrichPid(pid);
    }

    // Birth carries no container scope — the chain's CgroupClassifier handles
    // that on the resolved pid. Delegate (returns nullopt) so the composite
    // falls through.
    [[nodiscard]] std::optional<ContainerInfo>
        resolveContainerForPid(qint32 pid) override
    {
        return m_resolver->resolveContainerForPid(pid);
    }

private:
    // Declaration order matters for teardown: the reader is destroyed first
    // (reverse order) so its thread is joined before the resolver it feeds.
    std::unique_ptr<BpfBirthResolver> m_resolver;
    std::unique_ptr<BpfBirthReader>   m_reader;
};

} // namespace qiftop::backend::linuximpl

#endif // QIFTOP_HAVE_BPF
