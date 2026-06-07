#include "NetlinkWorker.h"

#include <netlink/netlink.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>

#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>

#include <QHash>

static constexpr int kPollIntervalMs = 1000;

namespace {

// Best-effort classification using libnl-3's kind string + ARPHRD type + flags.
QString classifyLink(rtnl_link *link, bool isLoopback)
{
    if (isLoopback)
        return QStringLiteral("loopback");

    if (const char *kind = rtnl_link_get_type(link); kind && *kind)
        return QString::fromLatin1(kind);

    switch (rtnl_link_get_arptype(link)) {
    case ARPHRD_ETHER:    return QStringLiteral("ethernet");
    case ARPHRD_PPP:      return QStringLiteral("ppp");
    case ARPHRD_TUNNEL:   return QStringLiteral("ipip");
    case ARPHRD_TUNNEL6:  return QStringLiteral("ip6tnl");
    case ARPHRD_SIT:      return QStringLiteral("sit");
    case ARPHRD_IEEE80211: return QStringLiteral("wifi");
    case ARPHRD_NONE:     return QStringLiteral("tun");
    default:              return QStringLiteral("other");
    }
}

QString formatAddrCidr(rtnl_addr *addr)
{
    nl_addr *local = rtnl_addr_get_local(addr);
    if (!local)
        return {};

    char buf[INET6_ADDRSTRLEN + 1] = {};
    nl_addr2str(local, buf, sizeof(buf));

    QString s = QString::fromLatin1(buf);
    // libnl already formats as "addr/prefix" most of the time. Make sure.
    if (!s.contains(QLatin1Char('/')))
        s += QStringLiteral("/%1").arg(rtnl_addr_get_prefixlen(addr));
    return s;
}

} // namespace

NetlinkWorker::NetlinkWorker(QObject *parent)
    : QObject(parent)
{}

NetlinkWorker::~NetlinkWorker()
{
    stop();
}

void NetlinkWorker::start()
{
    m_sock = nl_socket_alloc();
    if (!m_sock) {
        qWarning("NetlinkWorker: failed to allocate netlink socket");
        return;
    }

    if (nl_connect(m_sock, NETLINK_ROUTE) < 0) {
        qWarning("NetlinkWorker: failed to connect netlink socket");
        nl_socket_free(m_sock);
        m_sock = nullptr;
        return;
    }

    if (rtnl_link_alloc_cache(m_sock, AF_UNSPEC, &m_linkCache) < 0) {
        qWarning("NetlinkWorker: failed to allocate link cache");
        stop();
        return;
    }

    if (rtnl_addr_alloc_cache(m_sock, &m_addrCache) < 0) {
        qWarning("NetlinkWorker: failed to allocate address cache");
        stop();
        return;
    }

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &NetlinkWorker::poll);
    m_timer->start(kPollIntervalMs);

    poll(); // emit an immediate first snapshot
}

void NetlinkWorker::stop()
{
    if (m_timer) {
        m_timer->stop();
        m_timer = nullptr;
    }
    if (m_addrCache) {
        nl_cache_free(m_addrCache);
        m_addrCache = nullptr;
    }
    if (m_linkCache) {
        nl_cache_free(m_linkCache);
        m_linkCache = nullptr;
    }
    if (m_sock) {
        nl_close(m_sock);
        nl_socket_free(m_sock);
        m_sock = nullptr;
    }
}

void NetlinkWorker::setPollIntervalMs(int ms)
{
    if (!m_timer) return;
    if (ms <= 0) {
        if (m_timer->isActive()) m_timer->stop();
    } else {
        m_timer->setInterval(ms);
        if (!m_timer->isActive()) m_timer->start();
    }
}

void NetlinkWorker::poll()
{
    if (!m_sock || !m_linkCache || !m_addrCache)
        return;

    nl_cache_refill(m_sock, m_linkCache);
    nl_cache_refill(m_sock, m_addrCache);

    // Build ifindex -> [CIDR] map, filtering link-local & host-scope addresses.
    QHash<int, QStringList> addrsByIfIndex;
    for (nl_object *obj = nl_cache_get_first(m_addrCache); obj;
         obj = nl_cache_get_next(obj))
    {
        auto *addr = reinterpret_cast<rtnl_addr *>(obj);
        const int scope = rtnl_addr_get_scope(addr);
        // Exclude link-local (RT_SCOPE_LINK), host-only (RT_SCOPE_HOST) and
        // anything more restrictive — we only want routable / site addresses.
        if (scope >= RT_SCOPE_LINK)
            continue;

        const QString cidr = formatAddrCidr(addr);
        if (!cidr.isEmpty())
            addrsByIfIndex[rtnl_addr_get_ifindex(addr)].append(cidr);
    }

    QList<InterfaceStats> stats;
    for (nl_object *obj = nl_cache_get_first(m_linkCache); obj;
         obj = nl_cache_get_next(obj))
    {
        auto *link = reinterpret_cast<rtnl_link *>(obj);
        const unsigned flags = rtnl_link_get_flags(link);
        const bool isLoopback = (flags & IFF_LOOPBACK) != 0;

        InterfaceStats s {
            .name       = QString::fromLocal8Bit(rtnl_link_get_name(link)),
            .type       = classifyLink(link, isLoopback),
            .mtu        = rtnl_link_get_mtu(link),
            .addresses  = addrsByIfIndex.value(rtnl_link_get_ifindex(link)),
            .rxBytes    = rtnl_link_get_stat(link, RTNL_LINK_RX_BYTES),
            .txBytes    = rtnl_link_get_stat(link, RTNL_LINK_TX_BYTES),
            .rxPackets  = rtnl_link_get_stat(link, RTNL_LINK_RX_PACKETS),
            .txPackets  = rtnl_link_get_stat(link, RTNL_LINK_TX_PACKETS),
            .isUp       = (flags & IFF_UP) != 0,
            .isLoopback = isLoopback,
        };
        stats.append(std::move(s));
    }

    emit statsUpdated(stats);
}
