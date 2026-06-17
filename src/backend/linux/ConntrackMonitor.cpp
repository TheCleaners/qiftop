#include "ConntrackMonitor.h"
#include "ConntrackOrient.h"
#include "FlowTopK.h"
#include "util/Logging.h"

#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QSet>
#include <QTimer>
#include <QtDebug>

#include <algorithm>
#include <list>

extern "C" {
#include <libnetfilter_conntrack/libnetfilter_conntrack.h>
}
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace {

constexpr std::size_t kRouteCacheMaxEntries = 8192;
constexpr std::size_t kIfIndexCacheMaxEntries = 256;

// Bound the in-process snapshot to the top-N flows by total bytes, mirroring
// the agent's ConnectionsService::kMaxConnections (keep these two in sync).
// On a busy router the conntrack table can hold 100k+ flows; emitting all of
// them every tick — and pinning that into the aggregator/model — is wasteful.
// We keep only the loudest 4096, collected via a K-bounded min-heap (see
// FlowTopK.h::admitFlowTopK) so the transient memory is O(K), not O(table).
// Hosts with fewer than 4096 flows (the overwhelming majority) see no change.
constexpr int kMaxInProcessFlows = 4096;

using AddrToIface = QHash<QHostAddress, QString>;

template <typename Key, typename Value>
class LruCache {
public:
    explicit LruCache(std::size_t maxEntries)
        : m_maxEntries(maxEntries)
    {
    }

    Value *find(const Key &key)
    {
        auto it = m_items.find(key);
        if (it == m_items.end())
            return nullptr;
        touch(it.value());
        return &it.value().value;
    }

    void insert(const Key &key, const Value &value)
    {
        auto it = m_items.find(key);
        if (it != m_items.end()) {
            it.value().value = value;
            touch(it.value());
            return;
        }

        if (m_maxEntries == 0)
            return;

        m_recent.push_front(key);
        m_items.insert(key, Entry{value, m_recent.begin()});
        if (m_items.size() > qsizetype(m_maxEntries))
            evictLeastRecent();
    }

private:
    struct Entry {
        Value value;
        typename std::list<Key>::iterator recentIt;
    };

    void touch(Entry &entry)
    {
        m_recent.splice(m_recent.begin(), m_recent, entry.recentIt);
    }

    void evictLeastRecent()
    {
        const Key &key = m_recent.back();
        m_items.remove(key);
        m_recent.pop_back();
    }

    std::size_t m_maxEntries;
    std::list<Key> m_recent;
    QHash<Key, Entry> m_items;
};

struct IfaceMap {
    AddrToIface byAddr;     // any local IP → ifname
};

// Build the address → interface name map from QNetworkInterface (which wraps
// netlink). Cheap enough to rebuild every poll; interfaces change rarely but
// addresses (esp. DHCP / IPv6 SLAAC) do come and go.
IfaceMap buildIfaceMap()
{
    IfaceMap m;
    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const auto &i : ifaces) {
        const QString name = i.name();
        for (const auto &e : i.addressEntries())
            m.byAddr.insert(e.ip(), name);
    }
    return m;
}

// Route-trick: ask the kernel which source address would be used to reach
// `dst`, by opening a UDP socket and connect()ing without sending. Returns
// the chosen source address or a null QHostAddress on failure. The kernel
// runs the full routing table, including policy routes — exactly what we
// want for forwarded traffic where neither endpoint is local.
QHostAddress routeSourceForDest(const QHostAddress &dst)
{
    const bool v6 = dst.protocol() == QAbstractSocket::IPv6Protocol;
    const int family = v6 ? AF_INET6 : AF_INET;
    int s = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (s < 0)
        return {};

    int rc;
    if (v6) {
        sockaddr_in6 sa{};
        sa.sin6_family = AF_INET6;
        sa.sin6_port   = htons(53);
        const Q_IPV6ADDR a = dst.toIPv6Address();
        std::memcpy(&sa.sin6_addr, &a, sizeof(a));
        rc = ::connect(s, reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
    } else {
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(53);
        sa.sin_addr.s_addr = htonl(dst.toIPv4Address());
        rc = ::connect(s, reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
    }
    if (rc < 0) { ::close(s); return {}; }

    sockaddr_storage ss{};
    socklen_t sl = sizeof(ss);
    if (::getsockname(s, reinterpret_cast<sockaddr *>(&ss), &sl) != 0) {
        ::close(s);
        return {};
    }
    ::close(s);

    if (ss.ss_family == AF_INET) {
        const auto *sa = reinterpret_cast<sockaddr_in *>(&ss);
        return QHostAddress(ntohl(sa->sin_addr.s_addr));
    }
    if (ss.ss_family == AF_INET6) {
        const auto *sa = reinterpret_cast<sockaddr_in6 *>(&ss);
        Q_IPV6ADDR a;
        std::memcpy(&a, &sa->sin6_addr, sizeof(a));
        return QHostAddress(a);
    }
    return {};
}

struct PollCtx {
    QList<Connection>            *out;
    const QSet<QHostAddress>     *localAddrs;
    const IfaceMap               *ifaceMap;
    LruCache<QHostAddress, QString> *routeCache;   // dst → ifname
    LruCache<QString, quint32>      *ifIndexCache; // ifname → ifindex (cached if_nametoindex)
    int                           cap  = 0;        // top-N keep limit (<=0 = unbounded)
    int                           seen = 0;        // total flows observed this dump
};

quint32 cachedIfIndex(LruCache<QString, quint32> *cache, const QString &name)
{
    if (name.isEmpty()) return 0;
    if (const quint32 *idx = cache->find(name)) return *idx;
    const unsigned idx = ::if_nametoindex(name.toLocal8Bit().constData());
    cache->insert(name, idx);
    return idx;
}

QString attributeIface(PollCtx *ctx, const QHostAddress &local, const QHostAddress &remote)
{
    // Fast path: the local endpoint is one of our addresses.
    if (auto it = ctx->ifaceMap->byAddr.constFind(local); it != ctx->ifaceMap->byAddr.cend())
        return it.value();

    // Forwarded / NAT / virt flow: neither side is a host address. Ask the
    // kernel which interface would be used to reach the remote, caching the
    // answer per-destination to keep syscall pressure low.
    if (remote.isNull())
        return {};
    if (const QString *iface = ctx->routeCache->find(remote))
        return *iface;
    const QHostAddress src = routeSourceForDest(remote);
    QString iface;
    if (!src.isNull()) {
        if (auto it = ctx->ifaceMap->byAddr.constFind(src); it != ctx->ifaceMap->byAddr.cend())
            iface = it.value();
    }
    ctx->routeCache->insert(remote, iface);
    return iface;
}

int nfctCallback(enum nf_conntrack_msg_type, struct nf_conntrack *ct, void *data)
{
    auto *ctx = static_cast<PollCtx *>(data);

    const uint8_t l3 = nfct_get_attr_u8(ct, ATTR_ORIG_L3PROTO);
    const uint8_t l4 = nfct_get_attr_u8(ct, ATTR_ORIG_L4PROTO);

    L4Proto proto = L4Proto::Unknown;
    switch (l4) {
        case IPPROTO_TCP:    proto = L4Proto::Tcp;    break;
        case IPPROTO_UDP:    proto = L4Proto::Udp;    break;
        case IPPROTO_ICMP:   proto = L4Proto::Icmp;   break;
        case IPPROTO_ICMPV6: proto = L4Proto::IcmpV6; break;
        default: return NFCT_CB_CONTINUE;
    }

    QHostAddress src, dst;
    if (l3 == AF_INET) {
        const uint32_t s = nfct_get_attr_u32(ct, ATTR_ORIG_IPV4_SRC);
        const uint32_t d = nfct_get_attr_u32(ct, ATTR_ORIG_IPV4_DST);
        src = QHostAddress(ntohl(s));
        dst = QHostAddress(ntohl(d));
    } else if (l3 == AF_INET6) {
        const auto *sb = static_cast<const Q_IPV6ADDR *>(nfct_get_attr(ct, ATTR_ORIG_IPV6_SRC));
        const auto *db = static_cast<const Q_IPV6ADDR *>(nfct_get_attr(ct, ATTR_ORIG_IPV6_DST));
        if (!sb || !db) return NFCT_CB_CONTINUE;
        src = QHostAddress(*sb);
        dst = QHostAddress(*db);
    } else {
        return NFCT_CB_CONTINUE;
    }

    quint16 sport = 0, dport = 0;
    if (proto == L4Proto::Tcp || proto == L4Proto::Udp) {
        sport = ntohs(nfct_get_attr_u16(ct, ATTR_ORIG_PORT_SRC));
        dport = ntohs(nfct_get_attr_u16(ct, ATTR_ORIG_PORT_DST));
    }

    const auto getU64 = [ct](enum nf_conntrack_attr a) -> quint64 {
        return nfct_attr_is_set(ct, a) ? nfct_get_attr_u64(ct, a) : 0;
    };
    const quint64 origBytes = getU64(ATTR_ORIG_COUNTER_BYTES);
    const quint64 replBytes = getU64(ATTR_REPL_COUNTER_BYTES);
    const quint64 origPkts  = getU64(ATTR_ORIG_COUNTER_PACKETS);
    const quint64 replPkts  = getU64(ATTR_REPL_COUNTER_PACKETS);

    Connection c = qiftop::backend::linux::orientConntrackFlow(src, dst,
                                                               sport, dport,
                                                               proto,
                                                               origBytes, replBytes,
                                                               origPkts, replPkts,
                                                               *ctx->localAddrs);
    c.iface   = attributeIface(ctx, c.local.address, c.remote.address);
    c.ifIndex = cachedIfIndex(ctx->ifIndexCache, c.iface);
    if (proto == L4Proto::Tcp && nfct_attr_is_set(ct, ATTR_TCP_STATE)) {
        const quint8 raw = nfct_get_attr_u8(ct, ATTR_TCP_STATE);
        // Clamp to known TcpState range; future kernel additions become None
        // rather than silently mis-decoded on the wire.
        c.tcpState = (raw <= quint8(TcpState::SynSent2))
                     ? static_cast<TcpState>(raw)
                     : TcpState::None;
    }
    ++ctx->seen;
    // Bounded top-K by bytes: fill to cap, then only admit a flow louder than
    // the current smallest, evicting that smallest. The emitted set is the cap
    // loudest flows (unordered — the aggregator/model sorts for display,
    // exactly as with the agent's capped snapshot). See FlowTopK.h.
    qiftop::backend::linux::admitFlowTopK(*ctx->out, c, ctx->cap);
    return NFCT_CB_CONTINUE;
}

} // namespace

// Worker runs in m_thread. Each tick performs a synchronous conntrack dump via
// libnetfilter_conntrack: NFCT_Q_DUMP makes the kernel stream every active
// flow through our callback. Per-flow byte/packet counters require the kernel
// accounting sysctl `net.netfilter.nf_conntrack_acct=1`; without it the
// counter attributes are absent and rates will read 0. Opening the conntrack
// handle requires CAP_NET_ADMIN (run as root or grant the capability).
class ConntrackMonitor::Worker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

public slots:
    void start()
    {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &Worker::poll);
        m_timer->start(1000);

        ensureAccountingEnabled();

        // Fire an immediate poll so the table populates without waiting a tick.
        QMetaObject::invokeMethod(this, &Worker::poll, Qt::QueuedConnection);
    }

    void stop()
    {
        if (m_timer) {
            m_timer->stop();
            m_timer->deleteLater();
            m_timer = nullptr;
        }
    }

    void setPollIntervalMs(int ms)
    {
        if (!m_timer) return;
        if (ms <= 0) {
            if (m_timer->isActive()) m_timer->stop();
        } else {
            m_timer->setInterval(ms);
            if (!m_timer->isActive()) m_timer->start();
        }
    }

signals:
    void connectionsUpdated(QList<Connection> connections);
    void permissionDenied(QString detail);
    void accountingUnavailable(QString detail);

private:
    // Kernel flow accounting (nf_conntrack_acct) must be on for the
    // CTA_COUNTERS_{ORIG,REPLY} attributes to be present. If it's off, try
    // to flip it ourselves (needs CAP_NET_ADMIN, which we have if conntrack
    // dumps work). If we can't, tell the UI so it can hint at the user.
    void ensureAccountingEnabled()
    {
        if (m_acctChecked) return;
        m_acctChecked = true;

        constexpr const char *kSysctl = "/proc/sys/net/netfilter/nf_conntrack_acct";
        QFile f(QString::fromLatin1(kSysctl));
        if (!f.open(QIODevice::ReadOnly)) {
            // No accounting sysctl present at all — unusual but possible on
            // stripped-down kernels. Surface as a hint and move on.
            emit accountingUnavailable(tr("kernel sysctl unavailable: %1")
                                           .arg(QString::fromLocal8Bit(std::strerror(errno))));
            return;
        }
        const QByteArray cur = f.readAll().trimmed();
        f.close();
        if (cur == "1") return; // already on, nothing to do

        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit accountingUnavailable(tr("kernel flow accounting is off and "
                "we lack permission to enable it (try: sudo sysctl -w "
                "net.netfilter.nf_conntrack_acct=1)"));
            return;
        }
        if (f.write("1\n") != 2) {
            emit accountingUnavailable(tr("failed to enable kernel flow accounting: %1")
                                           .arg(f.errorString()));
            f.close();
            return;
        }
        f.close();
        qCInfo(lcVerbose) << "ConntrackMonitor: enabled nf_conntrack_acct";
        // New flows from now on carry counters. Existing flows in the dump
        // will still show 0 until they're renewed.
    }

private slots:
    void poll()
    {
        QList<Connection> flows;
        flows.reserve(kMaxInProcessFlows);
        const IfaceMap ifaceMap = buildIfaceMap();
        QSet<QHostAddress> localAddrs;
        localAddrs.reserve(ifaceMap.byAddr.size());
        for (auto it = ifaceMap.byAddr.cbegin(); it != ifaceMap.byAddr.cend(); ++it)
            localAddrs.insert(it.key());

        struct nfct_handle *h = nfct_open(CONNTRACK, 0);
        if (!h) {
            if (!m_warnedOpen) {
                qWarning("ConntrackMonitor: nfct_open failed (need CAP_NET_ADMIN); "
                         "the Connections tab will stay empty.");
                emit permissionDenied(QString::fromLocal8Bit(std::strerror(errno)));
                m_warnedOpen = true;
            }
            emit connectionsUpdated(flows);
            return;
        }

        PollCtx ctx{&flows, &localAddrs, &ifaceMap, &m_routeCache,
                    &m_ifIndexCache, kMaxInProcessFlows, 0};
        nfct_callback_register(h, NFCT_T_ALL, &nfctCallback, &ctx);

        // Dump v4 and v6 in two separate queries. AF_UNSPEC is documented
        // as "all families" but in practice many libnetfilter_conntrack /
        // kernel combos only return AF_INET entries that way — which is
        // exactly why we never saw any IPv6 flows. Two explicit dumps are
        // the universally-supported workaround.
        for (uint32_t family : {uint32_t(AF_INET), uint32_t(AF_INET6)}) {
            const int rc = nfct_query(h, NFCT_Q_DUMP, &family);
            if (rc < 0) {
                if (!m_warnedDump) {
                    qWarning("ConntrackMonitor: nfct_query(NFCT_Q_DUMP, family=%u) failed: %s",
                             family, std::strerror(errno));
                    if (errno == EPERM || errno == EACCES)
                        emit permissionDenied(QString::fromLocal8Bit(std::strerror(errno)));
                    m_warnedDump = true;
                }
                // Don't break — the other family might still succeed.
            }
        }
        nfct_close(h);

        // One-time notice if the kernel table exceeded the cap — the user is
        // seeing only the loudest kMaxInProcessFlows flows (matches the agent).
        if (ctx.seen > kMaxInProcessFlows && !m_warnedCap) {
            qWarning("ConntrackMonitor: conntrack table has %d flows; capping the "
                     "in-process snapshot at the %d loudest by bytes (warning "
                     "once; suppressing repeats).",
                     ctx.seen, kMaxInProcessFlows);
            m_warnedCap = true;
        }

        emit connectionsUpdated(flows);
    }

private:
    QTimer *m_timer       = nullptr;
    bool    m_warnedOpen  = false;
    bool    m_warnedDump  = false;
    bool    m_warnedCap   = false;
    bool    m_acctChecked = false;

    // dst-address → ifname cache for the route-lookup fallback (forwarded
    // flows). Bounded LRU so it can't grow unboundedly on a busy router.
    // Entries persist until evicted; this keeps the existing cached-across-
    // ticks staleness tradeoff while avoiding whole-cache thrash.
    LruCache<QHostAddress, QString> m_routeCache{kRouteCacheMaxEntries};
    // ifname → ifindex cache, shared across the whole tick. Avoids one
    // if_nametoindex() syscall per flow.
    LruCache<QString, quint32>      m_ifIndexCache{kIfIndexCacheMaxEntries};
};

ConntrackMonitor::ConntrackMonitor(QObject *parent)
    : ConnectionMonitor(parent)
    , m_worker(new Worker)
{
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::started,  m_worker, &Worker::start);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &Worker::connectionsUpdated,
            this,     &ConnectionMonitor::connectionsUpdated);
    connect(m_worker, &Worker::permissionDenied,
            this,     &ConnectionMonitor::permissionDenied);
    connect(m_worker, &Worker::accountingUnavailable,
            this,     &ConnectionMonitor::accountingUnavailable);
}

ConntrackMonitor::~ConntrackMonitor()
{
    ConntrackMonitor::stop();
}

void ConntrackMonitor::start()
{
    m_thread.start();
}

void ConntrackMonitor::stop()
{
    if (!m_thread.isRunning())
        return;
    QMetaObject::invokeMethod(m_worker, &Worker::stop, Qt::QueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void ConntrackMonitor::setPollIntervalMs(int ms)
{
    if (!m_thread.isRunning())
        return;
    QMetaObject::invokeMethod(m_worker, "setPollIntervalMs",
                              Qt::QueuedConnection, Q_ARG(int, ms));
}

QStringList ConntrackMonitor::capabilities() const
{
    // Structural tokens the conntrack dump actually delivers: proto (mapped
    // to IANA on the wire / kept as L4Proto in-process) and the TCP
    // conntrack state (ATTR_TCP_STATE). Direction and reason are inferred
    // client-side, not here, and there's no resolver so no attribution.
    return {
        QStringLiteral("iana-proto"),
        QStringLiteral("tcp-state"),
    };
}

#include "ConntrackMonitor.moc"
