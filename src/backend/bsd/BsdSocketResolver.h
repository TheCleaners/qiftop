#pragma once

#include <QHash>

#include "backend/Connection.h"
#include "backend/ProcessResolver.h" // ProcessInfo

namespace qiftop::backend::bsd {

// Pure-sysctl socket→process resolver for the BSDs. Reproduces what
// sockstat(1)/fstat(1) do without kvm: it joins
//   * KERN_FILE2/KERN_FILE_BYFILE  (kinfo_file: socket ptr ki_fdata → pid), and
//   * net.inet.{tcp,udp}.pcblist / net.inet6.{tcp6,udp6}.pcblist
//     (kinfo_pcb: socket ptr ki_sockaddr → local/remote 5-tuple),
// then enriches the pid via KERN_PROC2 (comm + uid). The result is a
// 5-tuple → ProcessInfo map the capture worker stamps onto flows.
//
// Currently NetBSD-specific (struct layouts are exported ABI but differ on
// FreeBSD/OpenBSD); guarded by __NetBSD__ in the .cpp so the BSD backend
// still builds on the others (attribution simply stays empty there).
class BsdSocketResolver {
public:
    // Rebuild the map from a fresh set of sysctls. Cheap enough to call once
    // per snapshot tick; the cost amortises against the conntrack-free design.
    void refresh();

    // Exact 4-tuple match first, then the local 2-tuple fallback. Returns an
    // invalid ProcessInfo (pid==0) when the flow can't be attributed.
    [[nodiscard]] ProcessInfo lookup(L4Proto proto, const Endpoint &local,
                                     const Endpoint &remote) const;

private:
    QHash<QString, ProcessInfo> m_exact;
    QHash<QString, ProcessInfo> m_local;
};

} // namespace qiftop::backend::bsd
