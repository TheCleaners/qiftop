// bpf-eval conntrack collector.
//
// Runs the PRODUCTION ConntrackMonitor (same 1 s periodic dump, same top-K
// cap, same nf_conntrack_acct handling, same attribution chain) and writes
// every snapshot's flows as NDJSON, stamped with an absolute CLOCK_MONOTONIC
// millisecond counter so the scorer can line them up against the generator's
// ground truth. This is deliberately the real code path, not a re-imagining —
// we measure what users actually get (DESIGN.md §5).
//
// Output: one NDJSON object per flow per snapshot:
//   {"ts_ms":..., "proto":"tcp", "local_ip":..., "local_port":...,
//    "remote_ip":..., "remote_port":..., "rx":..., "tx":...,
//    "dir":"outbound|inbound|unknown", "reason":..., "pid":...}
//
// Needs CAP_NET_ADMIN (conntrack) at runtime; the orchestrator runs it as root
// and SKIPs cleanly when unavailable.

#include <chrono>
#include <cstdio>
#include <memory>

#include <QCoreApplication>
#include <QHostAddress>
#include <QSet>
#include <QTimer>

#include "backend/Connection.h"
#include "backend/PlatformInfo.h"
#include "backend/linux/ConntrackMonitor.h"
#include "util/ConnectionHeuristics.h"

namespace {

long long monoMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char *dirStr(Direction d)
{
    switch (d) {
    case Direction::Outbound: return "outbound";
    case Direction::Inbound:  return "inbound";
    default:                  return "unknown";
    }
}

const char *protoStr(L4Proto p)
{
    switch (p) {
    case L4Proto::Tcp: return "tcp";
    case L4Proto::Udp: return "udp";
    default:           return "other";
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    int     pollMs       = 1000;   // production default
    int     durationMs   = 8000;
    QString outPath      = QStringLiteral("conntrack.ndjson");
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        auto nx = [&]() -> QString { return (i + 1 < args.size()) ? args.at(++i) : QString(); };
        if      (a == QLatin1String("--poll-ms"))     pollMs = nx().toInt();
        else if (a == QLatin1String("--duration-ms")) durationMs = nx().toInt();
        else if (a == QLatin1String("--out"))         outPath = nx();
    }

    FILE *out = std::fopen(outPath.toLocal8Bit().constData(), "we");
    if (!out) { std::perror("collector: open out"); return 1; }

    // Default ctor builds the production resolver chain, so attribution
    // (pid/container) is exercised exactly as in the agent's in-process path.
    auto monitor = std::make_unique<ConntrackMonitor>();
    monitor->setPollIntervalMs(pollMs);

    // Apply the SAME server-side direction + attribution-reason inference the
    // agent's ConnectionsService does, so we measure the conntrack path at
    // production parity (ConntrackMonitor itself emits Direction::Unknown —
    // inference is done at the service boundary). Host context is captured
    // once; local addresses don't change during a run.
    const QSet<QHostAddress> localAddrs    = qiftop::platform::localAddresses();
    const QSet<QHostAddress> loopbackAddrs = qiftop::platform::loopbackAddresses();
    const auto [ephLow, ephHigh]           = qiftop::platform::ephemeralPortRange();

    QObject::connect(monitor.get(), &ConnectionMonitor::connectionsUpdated,
                     [&, out](const QList<Connection> &conns) {
        const long long ts = monoMs();
        for (const Connection &raw : conns) {
            Connection c = raw;
            c.direction = qiftop::heuristics::inferDirection(
                c, localAddrs, loopbackAddrs, ephLow, ephHigh);
            c.reason = qiftop::heuristics::attributionReason(
                c, localAddrs, loopbackAddrs);
            std::fprintf(out,
                "{\"ts_ms\":%lld,\"proto\":\"%s\",\"local_ip\":\"%s\","
                "\"local_port\":%u,\"remote_ip\":\"%s\",\"remote_port\":%u,"
                "\"rx\":%llu,\"tx\":%llu,\"dir\":\"%s\",\"reason\":%d,\"pid\":%d}\n",
                ts, protoStr(c.proto),
                c.local.address.toString().toLocal8Bit().constData(), c.local.port,
                c.remote.address.toString().toLocal8Bit().constData(), c.remote.port,
                static_cast<unsigned long long>(c.rxBytes),
                static_cast<unsigned long long>(c.txBytes),
                dirStr(c.direction), static_cast<int>(c.reason), c.process.pid);
        }
        std::fflush(out);
    });

    monitor->start();

    QTimer::singleShot(durationMs, &app, [&app] { app.quit(); });
    const int rc = app.exec();

    monitor->stop();
    std::fclose(out);
    return rc;
}
