// Direction-inference heuristic. Pure, table-driven.

#include <QHostAddress>
#include <QSet>
#include <QtTest/QtTest>

#include "backend/Connection.h"
#include "util/ConnectionHeuristics.h"

using qiftop::heuristics::inferDirection;

namespace {

constexpr quint16 kEphLow  = 32768;
constexpr quint16 kEphHigh = 60999;

QSet<QHostAddress> loopback()
{
    return { QHostAddress(QHostAddress::LocalHost),
             QHostAddress(QHostAddress::LocalHostIPv6) };
}

Connection makeFlow(L4Proto p, const char *l, quint16 lp, const char *r, quint16 rp)
{
    Connection c;
    c.proto         = p;
    c.local.address = QHostAddress(QString::fromLatin1(l));
    c.local.port    = lp;
    c.remote.address = QHostAddress(QString::fromLatin1(r));
    c.remote.port    = rp;
    return c;
}

} // namespace

class TestDirection : public QObject {
    Q_OBJECT
private slots:
    void ephemeral_outbound()
    {
        // Local ephemeral src → remote 443 ⇒ Outbound.
        const auto c = makeFlow(L4Proto::Tcp, "192.168.1.10", 51234, "8.8.8.8", 443);
        QCOMPARE(inferDirection(c, {}, loopback(), kEphLow, kEphHigh),
                 Direction::Outbound);
    }

    void ephemeral_inbound()
    {
        // We're listening on 22, remote uses ephemeral 51234 ⇒ Inbound.
        const auto c = makeFlow(L4Proto::Tcp, "192.168.1.10", 22, "10.0.0.5", 51234);
        QCOMPARE(inferDirection(c, {}, loopback(), kEphLow, kEphHigh),
                 Direction::Inbound);
    }

    void icmp_is_unknown()
    {
        Connection c; c.proto = L4Proto::Icmp;
        c.local.address  = QHostAddress("10.0.0.1");
        c.remote.address = QHostAddress("8.8.8.8");
        QCOMPARE(inferDirection(c, {}, loopback(), kEphLow, kEphHigh),
                 Direction::Unknown);
    }

    void both_ephemeral_is_unknown()
    {
        // Both sides in ephemeral range and no local-addr hint ⇒ Unknown.
        const auto c = makeFlow(L4Proto::Tcp, "1.2.3.4", 40000, "5.6.7.8", 50000);
        QCOMPARE(inferDirection(c, {}, loopback(), kEphLow, kEphHigh),
                 Direction::Unknown);
    }

    void mdns_local_to_multicast_is_outbound()
    {
        // Both ports 5353 (mDNS): ephemeral heuristic punts → fallback uses
        // local-addr membership. Local end is one of our iface addrs.
        QSet<QHostAddress> local{ QHostAddress("172.20.20.20") };
        const auto c = makeFlow(L4Proto::Udp, "172.20.20.20", 5353, "224.0.0.251", 5353);
        QCOMPARE(inferDirection(c, local, loopback(), kEphLow, kEphHigh),
                 Direction::Outbound);
    }

    void dhcp_client_is_outbound()
    {
        // DHCP DISCOVER/REQUEST: client 68 → server 67.
        QSet<QHostAddress> local{ QHostAddress("172.20.20.20") };
        const auto c = makeFlow(L4Proto::Udp, "172.20.20.20", 68, "172.20.20.1", 67);
        QCOMPARE(inferDirection(c, local, loopback(), kEphLow, kEphHigh),
                 Direction::Outbound);
    }

    void ntp_forwarded_stays_unknown()
    {
        // NAT'd NTP through this host: neither end local ⇒ Unknown
        // (caller is expected to detect this as "forwarded" separately).
        QSet<QHostAddress> local{ QHostAddress("192.168.1.10") };
        const auto c = makeFlow(L4Proto::Udp, "10.42.0.158", 123, "108.61.73.244", 123);
        QCOMPARE(inferDirection(c, local, loopback(), kEphLow, kEphHigh),
                 Direction::Unknown);
    }

    void loopback_both_ends_stays_unknown()
    {
        // 127.0.0.1:443 → 127.0.0.1:51234 has ephemeral hint though, so
        // make BOTH sides well-known to exercise the both-local branch.
        const auto c = makeFlow(L4Proto::Tcp, "127.0.0.1", 80, "127.0.0.1", 443);
        QCOMPARE(inferDirection(c, {}, loopback(), kEphLow, kEphHigh),
                 Direction::Unknown);
    }

    void ephemeral_beats_local_addr_hint()
    {
        // When the ephemeral heuristic decides, it wins over the fallback
        // (the fallback only runs if neither port is ephemeral).
        QSet<QHostAddress> local{ QHostAddress("192.168.1.10") };
        const auto c = makeFlow(L4Proto::Tcp, "192.168.1.10", 22, "192.168.1.10", 51234);
        QCOMPARE(inferDirection(c, local, loopback(), kEphLow, kEphHigh),
                 Direction::Inbound);
    }
};

QTEST_APPLESS_MAIN(TestDirection)
#include "test_direction.moc"
