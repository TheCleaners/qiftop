// Unit tests for the Connections filter mini-language.
//
// Covers parser (success cases, precedence, byte suffixes, error
// positions), and evaluator (each field type, each operator, regex,
// "port" either-end semantics, host/hostname fallback).

#include "util/ConnectionFilter.h"
#include "backend/Connection.h"

#include <QHostAddress>
#include <QString>
#include <QtTest/QtTest>

using qiftop::filter::Context;
using qiftop::filter::matches;
using qiftop::filter::parse;

namespace {

Connection mkConn(L4Proto p,
                  const char *localIp, quint16 lport,
                  const char *remoteIp, quint16 rport,
                  quint64 rx = 0, quint64 tx = 0,
                  const char *iface = "wlp1s0",
                  Direction d = Direction::Outbound)
{
    Connection c;
    c.proto              = p;
    c.local.address      = QHostAddress(QString::fromLatin1(localIp));
    c.local.port         = lport;
    c.remote.address     = QHostAddress(QString::fromLatin1(remoteIp));
    c.remote.port        = rport;
    c.rxBytes            = rx;
    c.txBytes            = tx;
    c.iface              = QString::fromLatin1(iface);
    c.direction          = d;
    return c;
}

} // namespace

class TestFilter : public QObject {
    Q_OBJECT
private slots:
    // ---- Parser: empty / whitespace -----------------------------------
    void emptyInputIsNullExpr()
    {
        QVERIFY(parse(QString{}).expr.get() == nullptr);
        QVERIFY(parse(QStringLiteral("   ")).expr.get() == nullptr);
        // Null expr means "match all"
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.1", 1234, "1.1.1.1", 443);
        Context ctx{c, 0, 0, {}, {}};
        QVERIFY(matches(parse(QString{}).expr, ctx));
    }

    // ---- Parser: errors -----------------------------------------------
    void parseErrors_data()
    {
        QTest::addColumn<QString>("input");
        QTest::newRow("dangling-and")  << "proto:tcp and";
        QTest::newRow("unknown-field") << "bogus:x";
        QTest::newRow("missing-rhs")   << "proto:";
        QTest::newRow("unbalanced")    << "(proto:tcp";
        QTest::newRow("bad-regex")     << "host~(unterminated";
    }
    void parseErrors()
    {
        QFETCH(QString, input);
        const auto r = parse(input);
        QVERIFY2(!r.error.isEmpty(),
                 qPrintable(QStringLiteral("expected error for: %1").arg(input)));
        QVERIFY(r.expr.get() == nullptr);
    }

    // ---- Evaluator: string fields -------------------------------------
    void protoField()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.1", 1234, "1.1.1.1", 443);
        Context ctx{c, 0, 0, {}, {}};
        QVERIFY(matches(QStringLiteral("proto:tcp"), ctx));
        QVERIFY(matches(QStringLiteral("proto=TCP"), ctx));  // case-insensitive
        QVERIFY(!matches(QStringLiteral("proto:udp"), ctx));
        QVERIFY(matches(QStringLiteral("proto!=udp"), ctx));
    }

    void srcDstHost()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.1", 1234, "1.1.1.1", 443);
        Context ctx{c, 0, 0, QStringLiteral("desktop.local"),
                    QStringLiteral("one.one.one.one")};
        // ':' = substring (case-insensitive)
        QVERIFY(matches(QStringLiteral("src:10.0"), ctx));
        QVERIFY(matches(QStringLiteral("dst:1.1.1.1"), ctx));
        // host = either side (text incl. hostname)
        QVERIFY(matches(QStringLiteral("host:one.one"), ctx));
        QVERIFY(matches(QStringLiteral("host:desktop"), ctx));
        QVERIFY(!matches(QStringLiteral("host:nope"), ctx));
    }

    void ifaceField()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.1", 1234, "1.1.1.1", 443,
                              0, 0, "wlp228s0");
        Context ctx{c, 0, 0, {}, {}};
        QVERIFY(matches(QStringLiteral("iface:wlp228"), ctx));
        QVERIFY(matches(QStringLiteral("iface=wlp228s0"), ctx));
        QVERIFY(!matches(QStringLiteral("iface=eth0"), ctx));
    }

    void regexOperator()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.1", 1234, "1.1.1.1", 443);
        Context ctx{c, 0, 0, {}, {}};
        QVERIFY(matches(QStringLiteral("src~^10\\."), ctx));
        QVERIFY(!matches(QStringLiteral("src~^192\\."), ctx));
    }

    // ---- Evaluator: numeric fields ------------------------------------
    void portFields()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443);
        Context ctx{c, 0, 0, {}, {}};
        QVERIFY(matches(QStringLiteral("dport=443"), ctx));
        QVERIFY(matches(QStringLiteral("sport=5000"), ctx));
        QVERIFY(!matches(QStringLiteral("sport=443"), ctx));
        // 'port' matches either end
        QVERIFY(matches(QStringLiteral("port=443"), ctx));
        QVERIFY(matches(QStringLiteral("port=5000"), ctx));
        QVERIFY(!matches(QStringLiteral("port=80"), ctx));
        // Numeric ':' is also equality
        QVERIFY(matches(QStringLiteral("dport:443"), ctx));
        // Comparison
        QVERIFY(matches(QStringLiteral("dport<1024"), ctx));
        QVERIFY(matches(QStringLiteral("sport>=5000"), ctx));
    }

    void byteSuffixes()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.1", 1234, "1.1.1.1", 443,
                              2 * 1024 * 1024, 0);
        Context ctx{c, 0, 0, {}, {}};
        QVERIFY(matches(QStringLiteral("bytes_in>1Mi"), ctx));   // 1048576
        QVERIFY(matches(QStringLiteral("bytes_in>=2Mi"), ctx));
        QVERIFY(matches(QStringLiteral("bytes_in<3Mi"), ctx));
        QVERIFY(matches(QStringLiteral("bytes_in>1M"),  ctx));   // 1000000
        // 'bytes' sums in+out
        QVERIFY(matches(QStringLiteral("bytes>=2Mi"), ctx));
    }

    void rateFields()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.1", 1234, "1.1.1.1", 443);
        Context ctx{c, /*rx*/ 1500.0, /*tx*/ 500.0, {}, {}};
        QVERIFY(matches(QStringLiteral("rate_in>1K"), ctx));
        QVERIFY(matches(QStringLiteral("rate_out<1K"), ctx));
        QVERIFY(matches(QStringLiteral("rate>=2K"), ctx));  // sum
    }

    // ---- Evaluator: family / direction --------------------------------
    void familyAndDirection()
    {
        Connection v4 = mkConn(L4Proto::Tcp, "10.0.0.1", 1, "1.1.1.1", 1,
                               0, 0, "wlan0", Direction::Outbound);
        Connection v6 = mkConn(L4Proto::Tcp, "::1", 1, "::1", 1,
                               0, 0, "lo", Direction::Inbound);
        Context cv4{v4, 0, 0, {}, {}};
        Context cv6{v6, 0, 0, {}, {}};
        QVERIFY(matches(QStringLiteral("family=v4"), cv4));
        QVERIFY(matches(QStringLiteral("family:v6"), cv6));
        QVERIFY(matches(QStringLiteral("direction=out"), cv4));
        QVERIFY(matches(QStringLiteral("direction:in"), cv6));
    }

    // ---- Boolean composition + precedence -----------------------------
    void precedence()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443);
        Context ctx{c, 0, 0, {}, {}};
        // AND binds tighter than OR
        QVERIFY(matches(QStringLiteral("proto:udp or proto:tcp and dport=443"), ctx));
        QVERIFY(!matches(QStringLiteral("(proto:udp or proto:tcp) and dport=80"), ctx));
        // NOT
        QVERIFY(matches(QStringLiteral("not proto:udp"), ctx));
        QVERIFY(matches(QStringLiteral("!proto:udp"), ctx));
        // Mixed
        QVERIFY(matches(QStringLiteral("proto:tcp and (dport=443 or dport=80)"), ctx));
    }

    // ---- Recursion cap (DoS hardening) --------------------------------
    void deeplyNestedExpressionIsRejected()
    {
        // 200 nested parens around a trivial predicate. Without the depth
        // cap this blows the UI thread's stack on recursion.
        QString expr;
        const QString pred = QStringLiteral("proto:tcp");
        for (int i = 0; i < 200; ++i) expr += QLatin1Char('(');
        expr += pred;
        for (int i = 0; i < 200; ++i) expr += QLatin1Char(')');
        const auto res = parse(expr);
        QVERIFY(!res.error.isEmpty());
        QVERIFY(res.error.contains(QStringLiteral("nested too deeply")));
    }

    void moderatelyNestedExpressionStillParses()
    {
        // Each layer of parens consumes ~4 recursion frames (Or→And→Not→Atom),
        // so the cap of 64 leaves room for ~15 nested parens in practice.
        // 10 is well within the budget.
        QString expr;
        const QString pred = QStringLiteral("proto:tcp");
        for (int i = 0; i < 10; ++i) expr += QLatin1Char('(');
        expr += pred;
        for (int i = 0; i < 10; ++i) expr += QLatin1Char(')');
        const auto res = parse(expr);
        QVERIFY2(res.error.isEmpty(), qPrintable(res.error));
        QVERIFY(res.expr != nullptr);
    }

    // ---- v0.4 attribution fields --------------------------------------

    void pidUidNumericFields()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.5", 8080, "1.2.3.4", 443);
        c.process.pid = 1234;
        c.process.uid = 33;
        Context ctx{c};

        QVERIFY( matches(QStringLiteral("pid=1234"),  ctx));
        QVERIFY(!matches(QStringLiteral("pid=9999"),  ctx));
        QVERIFY( matches(QStringLiteral("pid>1000"),  ctx));
        QVERIFY( matches(QStringLiteral("pid<=1234"), ctx));
        QVERIFY( matches(QStringLiteral("uid=33"),    ctx));
        QVERIFY( matches(QStringLiteral("uid!=0"),    ctx));
    }

    void commStringField()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.5", 8080, "1.2.3.4", 443);
        c.process.pid  = 1; // any non-zero so the row is "attributed"
        c.process.comm = QStringLiteral("nginx");
        Context ctx{c};

        QVERIFY( matches(QStringLiteral("comm:ng"),     ctx));
        QVERIFY( matches(QStringLiteral("comm=nginx"),  ctx));
        QVERIFY( matches(QStringLiteral("comm~^ng"),    ctx));
        QVERIFY(!matches(QStringLiteral("comm=apache"), ctx));
    }

    void runtimeField()
    {
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.5", 8080, "1.2.3.4", 443);
        c.container.runtime = QStringLiteral("docker");
        c.container.id      = QStringLiteral("af85275074f5");
        c.container.name    = QStringLiteral("web");
        Context ctx{c};

        QVERIFY( matches(QStringLiteral("runtime:docker"),  ctx));
        QVERIFY( matches(QStringLiteral("runtime=DOCKER"),  ctx)); // case-insensitive
        QVERIFY(!matches(QStringLiteral("runtime=podman"),  ctx));
    }

    void containerMatchesIdRuntimeOrName()
    {
        // `container` is the multi-haystack equivalent of `host` for the
        // attribution columns: substring matches across runtime + id +
        // name so the user types "container:web" without having to know
        // whether they're matching a name or an id.
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.5", 8080, "1.2.3.4", 443);
        c.container.runtime = QStringLiteral("docker");
        c.container.id      = QStringLiteral("af85275074f5");
        c.container.name    = QStringLiteral("web-frontend");
        Context ctx{c};

        QVERIFY( matches(QStringLiteral("container:web"),       ctx));
        QVERIFY( matches(QStringLiteral("container:af85"),      ctx));
        QVERIFY( matches(QStringLiteral("container:docker"),    ctx));
        QVERIFY( matches(QStringLiteral("container=web-frontend"), ctx));
        QVERIFY(!matches(QStringLiteral("container=missing"),   ctx));
    }

    void chainHasMatchesAnyAncestor()
    {
        // `chain_has` walks the OUTER→INNER nesting and matches if ANY
        // entry's runtime / id / name substring-matches the value.
        // Used for "show me everything that's somewhere under k8s".
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.5", 8080, "1.2.3.4", 443);
        c.containerChain = {
            qiftop::backend::ContainerInfo{
                QStringLiteral("kubernetes"),
                QStringLiteral("pod-uid-abc"), {}},
            qiftop::backend::ContainerInfo{
                QStringLiteral("containerd"),
                QStringLiteral("cid12345678"),
                QStringLiteral("workload")},
        };
        Context ctx{c};

        QVERIFY( matches(QStringLiteral("chain_has:kubernetes"), ctx));
        QVERIFY( matches(QStringLiteral("chain_has:containerd"), ctx));
        QVERIFY( matches(QStringLiteral("chain_has:workload"),   ctx));
        QVERIFY( matches(QStringLiteral("chain_has:cid123"),     ctx));
        QVERIFY(!matches(QStringLiteral("chain_has:docker"),     ctx));
    }

    void attributionFieldsDefaultToEmpty()
    {
        // Unattributed flow (default Connection): nothing matches any of
        // the new fields. Critically `pid=0` MUST match so "show me
        // unattributed flows" works as a filter idiom.
        Connection c = mkConn(L4Proto::Tcp, "10.0.0.5", 8080, "1.2.3.4", 443);
        Context ctx{c};

        QVERIFY( matches(QStringLiteral("pid=0"),         ctx));
        QVERIFY(!matches(QStringLiteral("pid>0"),         ctx));
        QVERIFY(!matches(QStringLiteral("comm:nginx"),    ctx));
        QVERIFY(!matches(QStringLiteral("runtime:docker"),ctx));
        QVERIFY(!matches(QStringLiteral("container:web"), ctx));
        QVERIFY(!matches(QStringLiteral("chain_has:k8s"), ctx));
    }
};

QTEST_MAIN(TestFilter)
#include "test_filter.moc"
