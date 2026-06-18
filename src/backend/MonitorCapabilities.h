#pragma once

#include <QSet>
#include <QStringList>

#include "backend/ConnectionMonitor.h"
#include "backend/NetworkMonitor.h"

// Transport-neutral capability helpers (AGENTS.md §4).
//
// Capabilities are a property of the ACTIVE backend, not of the DBus agent:
// each monitor reports the tokens its own data path delivers, and the client
// gates optional UI on the UNION of the live monitors' capabilities —
// whether that's the DBus agent proxy or an in-process Linux/BSD backend.
namespace qiftop::backend {

// Union of two capability lists, de-duplicated, preserving first-seen order
// (network tokens first, then connection tokens). Order is cosmetic — clients
// branch on token PRESENCE — but a stable order keeps the About dialog and
// status tooltip readable.
[[nodiscard]] inline QStringList mergeCapabilities(const QStringList &a,
                                                   const QStringList &b)
{
    QStringList out;
    QSet<QString> seen;
    out.reserve(a.size() + b.size());
    for (const QStringList &src : {a, b}) {
        for (const QString &tok : src) {
            if (!seen.contains(tok)) {
                seen.insert(tok);
                out << tok;
            }
        }
    }
    return out;
}

// Convenience: union of the two active monitors' advertised capabilities.
[[nodiscard]] inline QStringList mergeCapabilities(const NetworkMonitor *net,
                                                   const ConnectionMonitor *conn)
{
    return mergeCapabilities(net  ? net->capabilities()  : QStringList{},
                             conn ? conn->capabilities() : QStringList{});
}

// Map a ProcessResolver's advertised capability tokens to the wire-level
// `*-attribution-wire` tokens a backend should advertise when that resolver
// is wired in. Shared by the DBus agent (InterfacesService) and the
// in-process Linux ConntrackMonitor so both derive the same wire contract
// from the same resolver caps (AGENTS.md §4):
//   process-attribution                      → process-attribution-wire
//   container-attribution                    → container-attribution-wire
//   container-attribution + container-chain  → container-chain-wire
// chain-wire is a strict superset of leaf container info, so it requires
// BOTH resolver tokens. Returns tokens in a stable order; callers append
// them to (and dedup against) their structural token list.
[[nodiscard]] inline QStringList
attributionWireTokens(const QStringList &resolverCaps)
{
    QStringList out;
    const bool process   = resolverCaps.contains(QStringLiteral("process-attribution"));
    const bool container = resolverCaps.contains(QStringLiteral("container-attribution"));
    const bool chain     = resolverCaps.contains(QStringLiteral("container-chain"));
    if (process)              out << QStringLiteral("process-attribution-wire");
    if (container)            out << QStringLiteral("container-attribution-wire");
    if (container && chain)   out << QStringLiteral("container-chain-wire");
    return out;
}

} // namespace qiftop::backend
