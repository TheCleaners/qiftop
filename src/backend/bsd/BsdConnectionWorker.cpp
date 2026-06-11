#include "BsdConnectionWorker.h"
#include "util/ConnectionHeuristics.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <ifaddrs.h>

#include <pcap.h>

#include <QDateTime>
#include <QSocketNotifier>
#include <QtEndian>

#include "util/Logging.h"

namespace qiftop::backend::bsd {

namespace {

constexpr int    kSnapLen        = 128;   // enough for L2 + IPv6 + TCP headers
constexpr int    kCaptureTimeout = 100;   // pcap to_ms
constexpr qint64 kFlowTtlMs      = 30000; // prune flows unseen this long

// Read an L4 (TCP/UDP) port pair from the start of the transport header.
bool readPorts(const quint8 *p, quint32 avail, quint16 *sport, quint16 *dport)
{
    if (avail < 4)
        return false;
    *sport = qFromBigEndian<quint16>(p);
    *dport = qFromBigEndian<quint16>(p + 2);
    return true;
}

L4Proto protoFromIp(quint8 ipproto)
{
    return fromIanaProto(ipproto);
}

// Stable ordering used to normalise both-local / forwarded flows so the two
// directions collapse to one entry. Higher port (the ephemeral client side)
// sorts as "local"; ties break on address string.
bool endpointGreater(const Endpoint &a, const Endpoint &b)
{
    if (a.port != b.port)
        return a.port > b.port;
    return a.address.toString() > b.address.toString();
}

QString flowKey(L4Proto proto, const Endpoint &local, const Endpoint &remote)
{
    return QStringLiteral("%1|%2.%3|%4.%5")
        .arg(static_cast<int>(proto))
        .arg(local.address.toString()).arg(local.port)
        .arg(remote.address.toString()).arg(remote.port);
}

} // namespace

BsdConnectionWorker::BsdConnectionWorker(QObject *parent)
    : QObject(parent)
{}

BsdConnectionWorker::~BsdConnectionWorker()
{
    stop();
}

void BsdConnectionWorker::refreshLocalState()
{
    m_localAddrs.clear();
    m_loopbackAddrs.clear();

    ifaddrs *ifap = nullptr;
    if (getifaddrs(&ifap) == 0 && ifap) {
        for (ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr)
                continue;
            const bool loop = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
            const int fam = ifa->ifa_addr->sa_family;
            if (fam == AF_INET) {
                const auto *sin = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
                QHostAddress a(qFromBigEndian<quint32>(&sin->sin_addr.s_addr));
                (loop ? m_loopbackAddrs : m_localAddrs).insert(a);
            } else if (fam == AF_INET6) {
                const auto *sin6 = reinterpret_cast<sockaddr_in6 *>(ifa->ifa_addr);
                Q_IPV6ADDR raw;
                memcpy(&raw, &sin6->sin6_addr, 16);
                QHostAddress a(raw);
                (loop ? m_loopbackAddrs : m_localAddrs).insert(a);
            }
        }
        freeifaddrs(ifap);
    }

    // Ephemeral port range (best effort; defaults already set).
    int v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname("net.inet.ip.portrange.first", &v, &len, nullptr, 0) == 0 && v > 0)
        m_ephLow = static_cast<quint16>(v);
    len = sizeof(v);
    if (sysctlbyname("net.inet.ip.portrange.last", &v, &len, nullptr, 0) == 0 && v > 0)
        m_ephHigh = static_cast<quint16>(v);
}

void BsdConnectionWorker::openCaptures()
{
    ifaddrs *ifap = nullptr;
    if (getifaddrs(&ifap) != 0 || !ifap) {
        if (ifap) freeifaddrs(ifap);
        return;
    }

    QSet<QString> seen;
    for (ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || !ifa->ifa_addr)
            continue;
        if (ifa->ifa_addr->sa_family != AF_LINK)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;
        const QString name = QString::fromUtf8(ifa->ifa_name);
        if (seen.contains(name))
            continue;
        seen.insert(name);

        char errbuf[PCAP_ERRBUF_SIZE] = {};
        pcap_t *p = pcap_open_live(ifa->ifa_name, kSnapLen, /*promisc=*/0,
                                   kCaptureTimeout, errbuf);
        if (!p) {
            qCInfo(lcVerbose).noquote()
                << "bsd-capture: skip" << name << ":" << errbuf;
            continue;
        }
        pcap_setnonblock(p, 1, errbuf);
        const int fd = pcap_get_selectable_fd(p);
        if (fd < 0) {
            qCInfo(lcVerbose).noquote()
                << "bsd-capture: no selectable fd on" << name;
            pcap_close(p);
            continue;
        }
        CaptureHandle h;
        h.pcap     = p;
        h.datalink = pcap_datalink(p);
        h.iface    = name;
        h.ifIndex  = if_nametoindex(ifa->ifa_name);
        m_handles.append(h);
    }
    freeifaddrs(ifap);

    // Wire up readable notifiers AFTER the list is fully built so the stored
    // CaptureHandle pointers don't dangle through reallocation.
    for (CaptureHandle &h : m_handles) {
        const int fd = pcap_get_selectable_fd(h.pcap);
        h.notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(h.notifier, &QSocketNotifier::activated,
                this, &BsdConnectionWorker::onReadable);
    }
}

void BsdConnectionWorker::closeCaptures()
{
    for (CaptureHandle &h : m_handles) {
        if (h.notifier) {
            h.notifier->setEnabled(false);
            delete h.notifier;
            h.notifier = nullptr;
        }
        if (h.pcap) {
            pcap_close(h.pcap);
            h.pcap = nullptr;
        }
    }
    m_handles.clear();
}

void BsdConnectionWorker::start()
{
    refreshLocalState();
    openCaptures();

    if (m_handles.isEmpty() && !m_warned) {
        m_warned = true;
        emit accountingUnavailable(
            QStringLiteral("Per-flow capture could not start (need read access "
                           "to /dev/bpf*; run with elevated privileges)."));
        emit connectionsUpdated({});
        return;
    }

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &BsdConnectionWorker::emitSnapshot);
    m_timer->start(1000);
    emitSnapshot();
}

void BsdConnectionWorker::stop()
{
    if (m_timer) {
        m_timer->stop();
        m_timer = nullptr;
    }
    closeCaptures();
    m_flows.clear();
}

void BsdConnectionWorker::setPollIntervalMs(int ms)
{
    if (!m_timer) return;
    if (ms <= 0) {
        if (m_timer->isActive()) m_timer->stop();
    } else {
        m_timer->setInterval(ms);
        if (!m_timer->isActive()) m_timer->start();
    }
}

void BsdConnectionWorker::onReadable(int fd)
{
    const CaptureHandle *target = nullptr;
    for (const CaptureHandle &h : m_handles) {
        if (h.pcap && pcap_get_selectable_fd(h.pcap) == fd) {
            target = &h;
            break;
        }
    }
    if (!target)
        return;

    struct Trampoline {
        static void cb(u_char *user, const struct pcap_pkthdr *hdr,
                       const u_char *bytes)
        {
            auto *ctx = reinterpret_cast<std::pair<BsdConnectionWorker *,
                                                    const CaptureHandle *> *>(user);
            ctx->first->handlePacket(*ctx->second,
                                     reinterpret_cast<const quint8 *>(bytes),
                                     hdr->caplen, hdr->len);
        }
    };
    std::pair<BsdConnectionWorker *, const CaptureHandle *> ctx{this, target};
    pcap_dispatch(target->pcap, -1, &Trampoline::cb,
                  reinterpret_cast<u_char *>(&ctx));
}

void BsdConnectionWorker::handlePacket(const CaptureHandle &h, const quint8 *data,
                                       quint32 caplen, quint32 wireLen)
{
    // --- locate the L3 (IP) header for this link-layer type ---
    quint32 off = 0;
    switch (h.datalink) {
    case DLT_EN10MB: {
        if (caplen < 14) return;
        quint16 ethertype = qFromBigEndian<quint16>(data + 12);
        off = 14;
        if (ethertype == 0x8100) { // 802.1Q VLAN
            if (caplen < 18) return;
            ethertype = qFromBigEndian<quint16>(data + 16);
            off = 18;
        }
        if (ethertype != 0x0800 && ethertype != 0x86DD)
            return;
        break;
    }
    case DLT_NULL:
    case DLT_LOOP:
        off = 4; // 4-byte address-family header
        break;
    default:
        off = 0; // DLT_RAW and friends: IP starts immediately
        break;
    }
    if (caplen <= off)
        return;

    // --- parse the IP header (version nibble drives the family) ---
    const quint8 *ip = data + off;
    const quint32 ipAvail = caplen - off;
    const quint8  version = ip[0] >> 4;

    Endpoint src, dst;
    quint8   ipproto = 0;
    quint32  l4off   = 0;

    if (version == 4) {
        if (ipAvail < 20) return;
        const quint32 ihl = (ip[0] & 0x0F) * 4u;
        if (ihl < 20 || ipAvail < ihl) return;
        ipproto = ip[9];
        src.address = QHostAddress(qFromBigEndian<quint32>(ip + 12));
        dst.address = QHostAddress(qFromBigEndian<quint32>(ip + 16));
        l4off = off + ihl;
    } else if (version == 6) {
        if (ipAvail < 40) return;
        ipproto = ip[6]; // next header (extension headers not chased)
        Q_IPV6ADDR s6, d6;
        memcpy(&s6, ip + 8, 16);
        memcpy(&d6, ip + 24, 16);
        src.address = QHostAddress(s6);
        dst.address = QHostAddress(d6);
        l4off = off + 40;
    } else {
        return;
    }

    const L4Proto proto = protoFromIp(ipproto);
    if (proto == L4Proto::Tcp || proto == L4Proto::Udp) {
        quint16 sp = 0, dp = 0;
        if (l4off < caplen && readPorts(data + l4off, caplen - l4off, &sp, &dp)) {
            src.port = sp;
            dst.port = dp;
        }
    }

    // --- orient: which side is "this host", and is this tx or rx? ---
    const auto isLocal = [&](const QHostAddress &a) {
        return m_localAddrs.contains(a) || m_loopbackAddrs.contains(a);
    };
    const bool ls = isLocal(src.address);
    const bool ld = isLocal(dst.address);

    Endpoint local, remote;
    bool isTx;
    if (ls && !ld) {            // outbound
        local = src; remote = dst; isTx = true;
    } else if (ld && !ls) {     // inbound
        local = dst; remote = src; isTx = false;
    } else {                    // both-local or forwarded: deterministic order
        if (endpointGreater(src, dst)) { local = src; remote = dst; isTx = true; }
        else                           { local = dst; remote = src; isTx = false; }
    }

    const QString key = flowKey(proto, local, remote);
    auto it = m_flows.find(key);
    if (it == m_flows.end()) {
        FlowAcc acc;
        acc.local = local; acc.remote = remote; acc.proto = proto;
        acc.iface = h.iface; acc.ifIndex = h.ifIndex;
        it = m_flows.insert(key, acc);
    }
    FlowAcc &acc = it.value();
    if (isTx) { acc.txBytes += wireLen; ++acc.txPackets; }
    else      { acc.rxBytes += wireLen; ++acc.rxPackets; }
    acc.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
}

void BsdConnectionWorker::emitSnapshot()
{
    refreshLocalState();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<Connection> out;
    out.reserve(m_flows.size());

    for (auto it = m_flows.begin(); it != m_flows.end(); ) {
        FlowAcc &a = it.value();
        if (now - a.lastSeenMs > kFlowTtlMs) {
            it = m_flows.erase(it);
            continue;
        }
        Connection c;
        c.local     = a.local;
        c.remote    = a.remote;
        c.proto     = a.proto;
        c.rxBytes   = a.rxBytes;
        c.txBytes   = a.txBytes;
        c.rxPackets = a.rxPackets;
        c.txPackets = a.txPackets;
        c.iface     = a.iface;
        c.ifIndex   = a.ifIndex;
        c.direction = qiftop::heuristics::inferDirection(
            c, m_localAddrs, m_loopbackAddrs, m_ephLow, m_ephHigh);
        out.append(std::move(c));
        ++it;
    }

    emit connectionsUpdated(out);
}

} // namespace qiftop::backend::bsd
