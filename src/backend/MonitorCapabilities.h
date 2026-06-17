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

} // namespace qiftop::backend
