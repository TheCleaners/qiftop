#pragma once

#include <QHash>

#include "backend/Connection.h"
#include "backend/ProcessResolver.h" // ProcessInfo, ContainerInfo

namespace qiftop::backend::bsd {

// Pure-sysctl socket→process (+ container) resolver for the BSDs. Maps a
// flow's 5-tuple to the owning process, and — where the platform has a
// container-like primitive — the enclosing scope, all without kvm:
//
//   * NetBSD: KERN_FILE2/KERN_FILE_BYPID (socket ptr → pid) joined with
//     net.inet.*.pcblist (socket ptr → 5-tuple) and KERN_PROC2 (pid →
//     comm/uid). No container model.
//   * FreeBSD: KERN_PROC_FILEDESC (kinfo_file carries kf_sa_local/peer and the
//     query is per-pid) + KERN_PROC_PROC (pid → comm/uid + ki_jid). Jailed
//     processes map to a ContainerInfo{runtime="jail", id=jid, name}.
//
// Other BSDs build with a no-op resolver (capture still works; flows
// unattributed). See docs/PORTABILITY.md §7.7.
class BsdSocketResolver {
public:
    // Process + (optional) container attribution for one flow.
    struct Attribution {
        ProcessInfo   process;
        ContainerInfo container;
    };

    // Rebuild the maps from a fresh set of sysctls. Cheap enough to call once
    // per snapshot tick; the cost amortises against the conntrack-free design.
    void refresh();

    // Exact 4-tuple match first, then the local 2-tuple fallback. Returns a
    // default Attribution (invalid process/container) when the flow can't be
    // attributed.
    [[nodiscard]] Attribution lookup(L4Proto proto, const Endpoint &local,
                                     const Endpoint &remote) const;

private:
    QHash<QString, Attribution> m_exact;
    QHash<QString, Attribution> m_local;
};

} // namespace qiftop::backend::bsd
