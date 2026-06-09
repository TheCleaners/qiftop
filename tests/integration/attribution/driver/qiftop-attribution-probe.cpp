// qiftop-attribution-probe
//
// Tier-2 attribution integration test driver. Constructs a process
// resolver via the production factory, queries it for an attribution
// of a single 4-tuple flow, and compares against expectations passed
// on the command line.
//
// See tests/integration/attribution/README.md for the harness design.
//
// Usage:
//   qiftop-attribution-probe \
//       --proto tcp|udp \
//       --local  <ip>:<port> \
//       --remote <ip>:<port> \
//       [--expect-pid <int>]
//       [--expect-pid-comm <string>]
//       [--expect-container-runtime <string>]
//       [--expect-container-id-prefix <hex>]
//       [--timeout-ms <int>]   (default 8000)
//       [--poll-ms <int>]      (default 250)
//       [--json]
//
// Exit codes:
//   0  every expectation passed
//   1  attribution found but at least one expectation failed
//   2  timeout — no attribution found within --timeout-ms
//   3  argv parse error
//
// Caller responsibilities:
//   - Run as root (or with CAP_NET_ADMIN + CAP_SYS_ADMIN) so SockDiagResolver
//     and NetnsScanner can do their work.
//   - Establish the flow BEFORE calling this probe.
//   - Pass the EXACT 4-tuple of the established flow (get it via `ss -tnp`
//     or by reading the conntrack table).

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTextStream>
#include <QThread>

#include "backend/Connection.h"
#include "backend/ProcessResolver.h"
#include "backend/ProcessResolverFactory.h"

namespace {

constexpr int kExitOk        = 0;
constexpr int kExitMismatch  = 1;
constexpr int kExitTimeout   = 2;
constexpr int kExitArgvError = 3;

QTextStream &cerr()
{
    static QTextStream s(stderr);
    return s;
}

bool parseEndpoint(const QString &arg, Endpoint &out, QString &err)
{
    // Accepts "ipv4:port" or "[ipv6]:port".
    const int colon = arg.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0 || colon == arg.size() - 1) {
        err = QStringLiteral("endpoint '%1' missing :port").arg(arg);
        return false;
    }
    QString host = arg.left(colon);
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']'))) {
        host = host.mid(1, host.size() - 2);
    }
    QHostAddress addr;
    if (!addr.setAddress(host)) {
        err = QStringLiteral("endpoint host '%1' is not a valid IP literal").arg(host);
        return false;
    }
    bool ok = false;
    const uint port = arg.mid(colon + 1).toUInt(&ok);
    if (!ok || port == 0 || port > 0xffff) {
        err = QStringLiteral("endpoint port '%1' invalid").arg(arg.mid(colon + 1));
        return false;
    }
    out.address = addr;
    out.port    = quint16(port);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qiftop-attribution-probe"));

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral(
        "Tier-2 attribution integration probe. Returns 0 iff the resolver "
        "produces an attribution for the given flow that matches every "
        "--expect-* expectation within --timeout-ms."));
    p.addHelpOption();
    p.addOption(QCommandLineOption(QStringLiteral("proto"),
        QStringLiteral("L4 protocol (tcp|udp)"), QStringLiteral("tcp|udp")));
    p.addOption(QCommandLineOption(QStringLiteral("local"),
        QStringLiteral("local endpoint as ip:port"), QStringLiteral("ip:port")));
    p.addOption(QCommandLineOption(QStringLiteral("remote"),
        QStringLiteral("remote endpoint as ip:port"), QStringLiteral("ip:port")));
    p.addOption(QCommandLineOption(QStringLiteral("expect-pid"),
        QStringLiteral("exact pid match"), QStringLiteral("pid")));
    p.addOption(QCommandLineOption(QStringLiteral("expect-pid-comm"),
        QStringLiteral("expected /proc/<pid>/status Name"), QStringLiteral("name")));
    p.addOption(QCommandLineOption(QStringLiteral("expect-container-runtime"),
        QStringLiteral("expected container runtime name"), QStringLiteral("name")));
    p.addOption(QCommandLineOption(QStringLiteral("expect-container-id-prefix"),
        QStringLiteral("expected container ID prefix (hex)"), QStringLiteral("hex")));
    p.addOption(QCommandLineOption(QStringLiteral("timeout-ms"),
        QStringLiteral("attribution timeout in ms"), QStringLiteral("ms"),
        QStringLiteral("8000")));
    p.addOption(QCommandLineOption(QStringLiteral("poll-ms"),
        QStringLiteral("poll interval in ms"), QStringLiteral("ms"),
        QStringLiteral("250")));
    p.addOption(QCommandLineOption(QStringLiteral("json")));
    p.process(app);

    if (!p.isSet(QStringLiteral("proto")) ||
        !p.isSet(QStringLiteral("local")) ||
        !p.isSet(QStringLiteral("remote"))) {
        cerr() << "missing required --proto / --local / --remote\n";
        return kExitArgvError;
    }

    L4Proto proto = L4Proto::Unknown;
    const QString protoStr = p.value(QStringLiteral("proto")).toLower();
    if      (protoStr == QLatin1String("tcp")) proto = L4Proto::Tcp;
    else if (protoStr == QLatin1String("udp")) proto = L4Proto::Udp;
    else {
        cerr() << "invalid --proto (expected tcp|udp): " << protoStr << "\n";
        return kExitArgvError;
    }

    Connection flow;
    flow.proto = proto;
    QString err;
    if (!parseEndpoint(p.value(QStringLiteral("local")),  flow.local,  err) ||
        !parseEndpoint(p.value(QStringLiteral("remote")), flow.remote, err)) {
        cerr() << err << "\n";
        return kExitArgvError;
    }

    const int  timeoutMs = p.value(QStringLiteral("timeout-ms")).toInt();
    const int  pollMs    = p.value(QStringLiteral("poll-ms")).toInt();
    const bool wantJson  = p.isSet(QStringLiteral("json"));

    auto resolver = qiftop::backend::createProcessResolver();
    const QStringList caps = resolver->capabilities();

    if (!wantJson) {
        cerr() << "probe: caps = " << caps.join(QLatin1Char(',')) << "\n";
    }

    QElapsedTimer t;
    t.start();
    std::optional<qiftop::backend::ProcessInfo>   proc;
    std::optional<qiftop::backend::ContainerInfo> ctr;
    const bool wantContainer =
        p.isSet(QStringLiteral("expect-container-runtime")) ||
        p.isSet(QStringLiteral("expect-container-id-prefix"));

    while (t.elapsed() < timeoutMs) {
        proc = resolver->resolveFlow(flow);
        if (proc) {
            ctr = resolver->resolveContainerForPid(proc->pid);
        }
        if (proc && (ctr || !wantContainer)) break;
        QThread::msleep(pollMs);
    }

    QJsonObject result;
    result.insert(QStringLiteral("elapsed_ms"), qint64(t.elapsed()));
    result.insert(QStringLiteral("capabilities"),
                  QJsonArray::fromStringList(caps));

    if (!proc) {
        result.insert(QStringLiteral("status"),  QStringLiteral("timeout"));
        result.insert(QStringLiteral("message"),
                      QStringLiteral("no attribution found within --timeout-ms"));
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
        return kExitTimeout;
    }

    result.insert(QStringLiteral("process"), QJsonObject{
        {QStringLiteral("pid"),     qint64(proc->pid)},
        {QStringLiteral("comm"),    proc->comm},
        {QStringLiteral("uid"),     qint64(proc->uid)},
        {QStringLiteral("cmdline"), proc->cmdline},
        {QStringLiteral("exe"),     proc->exe},
    });
    if (ctr) {
        result.insert(QStringLiteral("container"), QJsonObject{
            {QStringLiteral("runtime"), ctr->runtime},
            {QStringLiteral("id"),      ctr->id},
            {QStringLiteral("name"),    ctr->name},
        });
    }

    QStringList failures;
    if (p.isSet(QStringLiteral("expect-pid"))) {
        const qint32 want = p.value(QStringLiteral("expect-pid")).toInt();
        if (proc->pid != want)
            failures << QStringLiteral("pid: expected %1 got %2").arg(want).arg(proc->pid);
    }
    if (p.isSet(QStringLiteral("expect-pid-comm"))) {
        const QString want = p.value(QStringLiteral("expect-pid-comm"));
        if (proc->comm != want)
            failures << QStringLiteral("comm: expected '%1' got '%2'").arg(want, proc->comm);
    }
    if (p.isSet(QStringLiteral("expect-container-runtime"))) {
        const QString want = p.value(QStringLiteral("expect-container-runtime"));
        if (!ctr)
            failures << QStringLiteral("container.runtime: expected '%1' but no container info").arg(want);
        else if (ctr->runtime != want)
            failures << QStringLiteral("container.runtime: expected '%1' got '%2'").arg(want, ctr->runtime);
    }
    if (p.isSet(QStringLiteral("expect-container-id-prefix"))) {
        const QString want = p.value(QStringLiteral("expect-container-id-prefix"));
        if (!ctr)
            failures << QStringLiteral("container.id: expected prefix '%1' but no container info").arg(want);
        else if (!ctr->id.startsWith(want))
            failures << QStringLiteral("container.id: expected prefix '%1' got '%2'").arg(want, ctr->id);
    }

    if (!failures.isEmpty()) {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        QJsonArray fa;
        for (const auto &f : failures) fa.append(f);
        result.insert(QStringLiteral("failures"), fa);
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
        return kExitMismatch;
    }

    result.insert(QStringLiteral("status"), QStringLiteral("ok"));
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
    return kExitOk;
}
