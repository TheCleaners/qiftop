#pragma once

// Pure, header-only helpers extracted from ConnectionModel so they can
// be unit-tested without instantiating a Qt model / proxy / DNS stack.
// Anything here MUST stay free of QObject / signal-slot / event-loop
// dependencies — Qt value types (QHostAddress, QSet) are fine.

#include <QHostAddress>
#include <QSet>

#include "backend/Connection.h"

namespace qiftop::heuristics {

// Returns true iff neither end of the flow is one of "this host"'s own
// addresses (loopback or any local interface address). Such flows are
// being routed *through* this host (NAT/masquerade, IP forwarding) and
// the inbound/outbound concept relative to "us" is meaningless.
[[nodiscard]] inline bool isForwardedFlow(const Connection &c,
                                          const QSet<QHostAddress> &localAddrs,
                                          const QSet<QHostAddress> &loopbackAddrs)
{
    const auto isOurs = [&](const QHostAddress &a) {
        return loopbackAddrs.contains(a) || localAddrs.contains(a);
    };
    return !isOurs(c.local.address) && !isOurs(c.remote.address);
}

// Client-side direction inference. Two-stage:
//   1. Ephemeral-port heuristic (/proc/sys/net/ipv4/ip_local_port_range):
//      whichever side's port is in the ephemeral range initiated.
//   2. Well-known-on-both-sides fallback (e.g. mDNS 5353↔5353, DHCP
//      68↔67): if exactly one endpoint is local, that side initiated.
// Both-local stays Unknown; neither-local is a forwarded flow.
[[nodiscard]] inline Direction inferDirection(const Connection &c,
                                              const QSet<QHostAddress> &localAddrs,
                                              const QSet<QHostAddress> &loopbackAddrs,
                                              quint16 ephemeralLow,
                                              quint16 ephemeralHigh)
{
    if (c.proto != L4Proto::Tcp && c.proto != L4Proto::Udp)
        return Direction::Unknown;
    const bool lEph = c.local.port  >= ephemeralLow && c.local.port  <= ephemeralHigh;
    const bool rEph = c.remote.port >= ephemeralLow && c.remote.port <= ephemeralHigh;
    if (lEph && !rEph) return Direction::Outbound;
    if (rEph && !lEph) return Direction::Inbound;
    const bool lLocal = localAddrs.contains(c.local.address)
                        || loopbackAddrs.contains(c.local.address);
    const bool rLocal = localAddrs.contains(c.remote.address)
                        || loopbackAddrs.contains(c.remote.address);
    if (lLocal && !rLocal) return Direction::Outbound;
    if (rLocal && !lLocal) return Direction::Inbound;
    return Direction::Unknown;
}

// dt-aware exponential moving average. tauMs is the smoothing time
// constant in milliseconds; 0 means "no smoothing" (returns raw).
// dtMs ≤ 0 is treated as "no time elapsed" and also returns raw, so
// the very first sample of a series doesn't blend into stale zeros.
[[nodiscard]] inline double emaUpdate(double prev, double raw, double dtMs, double tauMs)
{
    if (tauMs <= 0.0 || dtMs <= 0.0)
        return raw;
    const double alpha = dtMs / (tauMs + dtMs);
    return alpha * raw + (1.0 - alpha) * prev;
}

// Cubic ease-out: f(0)=0, f(1)=1, fast start, smooth landing.
// Used to tween the display rate from its prior value to the new EMA
// target over a poll interval so changes animate rather than step.
// t is clamped to [0,1] by the caller.
[[nodiscard]] inline double easeOutCubic(double t)
{
    const double u = 1.0 - t;
    return 1.0 - u * u * u;
}

} // namespace qiftop::heuristics
