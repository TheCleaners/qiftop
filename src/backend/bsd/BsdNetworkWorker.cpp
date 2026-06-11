#include "BsdNetworkWorker.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include <QHash>

namespace qiftop::backend::bsd {

namespace {

constexpr int kPollIntervalMs = 1000;

// Map a BSD IFT_* media type (struct if_data::ifi_type) to qiftop's
// coarse interface-type string. Mirrors the categories the Linux
// backend emits so the frontends render the same labels cross-platform.
QString classifyType(unsigned char ifiType, bool isLoopback)
{
    if (isLoopback)
        return QStringLiteral("loopback");

    switch (ifiType) {
    case IFT_ETHER:    return QStringLiteral("ethernet");
    case IFT_LOOP:     return QStringLiteral("loopback");
#ifdef IFT_L2VLAN
    case IFT_L2VLAN:   return QStringLiteral("vlan");
#endif
#ifdef IFT_BRIDGE
    case IFT_BRIDGE:   return QStringLiteral("bridge");
#endif
#ifdef IFT_GIF
    case IFT_GIF:      return QStringLiteral("gif");
#endif
#ifdef IFT_TUNNEL
    case IFT_TUNNEL:   return QStringLiteral("tunnel");
#endif
#ifdef IFT_IEEE80211
    case IFT_IEEE80211: return QStringLiteral("wifi");
#endif
#ifdef IFT_PPP
    case IFT_PPP:      return QStringLiteral("ppp");
#endif
    default:           return QStringLiteral("other");
    }
}

// RFC 2863 IF_OPER_* mapping from the BSD link state. NetBSD exposes
// LINK_STATE_{UNKNOWN,DOWN,UP} in struct if_data::ifi_link_state.
quint8 operStateFromLink(int linkState)
{
    switch (linkState) {
    case LINK_STATE_UP:   return 6; // UP
    case LINK_STATE_DOWN: return 2; // DOWN
    default:              return 0; // UNKNOWN
    }
}

QString cidrFromSockaddr(const sockaddr *addr, const sockaddr *mask)
{
    if (!addr)
        return {};

    char host[INET6_ADDRSTRLEN] = {};
    int prefix = -1;

    if (addr->sa_family == AF_INET) {
        const auto *sin = reinterpret_cast<const sockaddr_in *>(addr);
        if (!inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)))
            return {};
        if (mask) {
            const auto *smask = reinterpret_cast<const sockaddr_in *>(mask);
            quint32 m = ntohl(smask->sin_addr.s_addr);
            prefix = 0;
            while (m & 0x80000000u) { ++prefix; m <<= 1; }
        }
    } else if (addr->sa_family == AF_INET6) {
        const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(addr);
        // Skip link-local (fe80::/10): not useful as a host address.
        if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))
            return {};
        if (!inet_ntop(AF_INET6, &sin6->sin6_addr, host, sizeof(host)))
            return {};
        if (mask) {
            const auto *smask = reinterpret_cast<const sockaddr_in6 *>(mask);
            prefix = 0;
            for (int i = 0; i < 16; ++i) {
                unsigned char b = smask->sin6_addr.s6_addr[i];
                while (b & 0x80) { ++prefix; b <<= 1; }
            }
        }
    } else {
        return {};
    }

    QString s = QString::fromLatin1(host);
    if (prefix >= 0)
        s += QStringLiteral("/%1").arg(prefix);
    return s;
}

} // namespace

BsdNetworkWorker::BsdNetworkWorker(QObject *parent)
    : QObject(parent)
{}

BsdNetworkWorker::~BsdNetworkWorker()
{
    stop();
}

void BsdNetworkWorker::start()
{
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &BsdNetworkWorker::poll);
    m_timer->start(kPollIntervalMs);
    poll(); // immediate first snapshot
}

void BsdNetworkWorker::stop()
{
    if (m_timer) {
        m_timer->stop();
        m_timer = nullptr;
    }
}

void BsdNetworkWorker::setPollIntervalMs(int ms)
{
    if (!m_timer) return;
    if (ms <= 0) {
        if (m_timer->isActive()) m_timer->stop();
    } else {
        m_timer->setInterval(ms);
        if (!m_timer->isActive()) m_timer->start();
    }
}

void BsdNetworkWorker::poll()
{
    ifaddrs *ifap = nullptr;
    if (getifaddrs(&ifap) != 0 || !ifap) {
        if (ifap) freeifaddrs(ifap);
        return;
    }

    // First pass: collect host addresses (AF_INET / AF_INET6) per ifname.
    QHash<QString, QStringList> addrsByName;
    for (ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_name)
            continue;
        const int fam = ifa->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6)
            continue;
        QString cidr = cidrFromSockaddr(ifa->ifa_addr, ifa->ifa_netmask);
        if (!cidr.isEmpty())
            addrsByName[QString::fromUtf8(ifa->ifa_name)].append(cidr);
    }

    // Second pass: one InterfaceStats per AF_LINK record (the per-interface
    // counters live in struct if_data hanging off the link-layer address).
    QList<InterfaceStats> out;
    for (ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_name)
            continue;
        if (ifa->ifa_addr->sa_family != AF_LINK)
            continue;
        const auto *data = static_cast<const if_data *>(ifa->ifa_data);
        if (!data)
            continue;

        const QString name = QString::fromUtf8(ifa->ifa_name);
        const bool isUp       = (ifa->ifa_flags & IFF_UP) != 0;
        const bool isLoopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;

        InterfaceStats s;
        s.name       = name;
        s.type       = classifyType(data->ifi_type, isLoopback);
        s.mtu        = static_cast<quint32>(data->ifi_mtu);
        s.addresses  = addrsByName.value(name);
        s.rxBytes    = data->ifi_ibytes;
        s.txBytes    = data->ifi_obytes;
        s.rxPackets  = data->ifi_ipackets;
        s.txPackets  = data->ifi_opackets;
        s.isUp       = isUp;
        s.isLoopback = isLoopback;
        s.ifIndex    = if_nametoindex(ifa->ifa_name);
        s.operState  = operStateFromLink(data->ifi_link_state);
        s.rxErrors   = data->ifi_ierrors;
        s.txErrors   = data->ifi_oerrors;
        s.rxDropped  = data->ifi_iqdrops;
        s.txDropped  = 0; // BSD if_data has no output-queue-drop counter
        out.append(std::move(s));
    }

    freeifaddrs(ifap);
    emit statsUpdated(out);
}

} // namespace qiftop::backend::bsd
