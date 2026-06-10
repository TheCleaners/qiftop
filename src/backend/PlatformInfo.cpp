#include "PlatformInfo.h"

#include <QFile>
#include <QHash>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QStringList>

#if defined(Q_OS_UNIX)
#  include <ifaddrs.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <pwd.h>
#  include <unistd.h>
#endif

namespace qiftop::platform {

namespace {

// IANA's recommendation per RFC 6056 §3.2. Used when the running OS
// either has no configurable ephemeral range (Windows < Vista) or we
// can't read it.
constexpr quint16 kIanaEphemeralLow  = 49152;
constexpr quint16 kIanaEphemeralHigh = 65535;

void gatherIfaddrs(QSet<QHostAddress> &local, QSet<QHostAddress> &loopback)
{
#if defined(Q_OS_UNIX)
    struct ifaddrs *head = nullptr;
    if (getifaddrs(&head) != 0) return;
    for (auto *p = head; p; p = p->ifa_next) {
        if (!p->ifa_addr) continue;
        const int fam = p->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6) continue;
        QHostAddress addr;
        addr.setAddress(p->ifa_addr);
        if (addr.isNull()) continue;
        if (addr.isLoopback()) loopback.insert(addr);
        else                   local.insert(addr);
    }
    freeifaddrs(head);
#else
    Q_UNUSED(local);
    Q_UNUSED(loopback);
    // TODO(port): on Windows use GetAdaptersAddresses() from iphlpapi.
#endif
}

} // namespace

QSet<QHostAddress> localAddresses()
{
    QSet<QHostAddress> local, loopback;
    gatherIfaddrs(local, loopback);
    return local;
}

QSet<QHostAddress> loopbackAddresses()
{
    QSet<QHostAddress> local, loopback;
    gatherIfaddrs(local, loopback);
    return loopback;
}

std::pair<quint16, quint16> ephemeralPortRange()
{
#if defined(Q_OS_LINUX)
    // sysctl net.ipv4.ip_local_port_range — two whitespace-separated ints.
    // The IPv6 stack reuses this range so a single read is enough.
    QFile portRange(QStringLiteral("/proc/sys/net/ipv4/ip_local_port_range"));
    if (portRange.open(QIODevice::ReadOnly)) {
        const auto parts = QString::fromLatin1(portRange.readAll())
                               .split(QRegularExpression(QStringLiteral("\\s+")),
                                      Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            bool ok1 = false, ok2 = false;
            const auto lo = parts[0].toUShort(&ok1);
            const auto hi = parts[1].toUShort(&ok2);
            if (ok1 && ok2 && hi > lo && lo >= 1024) {
                return {lo, hi};
            }
        }
    }
#endif
#if defined(Q_OS_DARWIN) || defined(Q_OS_FREEBSD)
    // TODO(port): sysctlbyname("net.inet.ip.portrange.first" /
    // "...portrange.last") on Darwin/FreeBSD.
#endif
    return {kIanaEphemeralLow, kIanaEphemeralHigh};
}

QString userNameForUid(uint uid)
{
#if defined(Q_OS_UNIX)
    // Small process-lifetime cache: uid→name mappings effectively never
    // change while we run, and getpwuid_r hits NSS (potentially LDAP/SSSD)
    // so we don't want it on every row render.
    static QReadWriteLock lock;
    static QHash<uint, QString> cache;
    // Bounded-cache discipline (AGENTS.md §8a rule 8): a flood of
    // distinct uids (e.g. attacker-controlled flows on a multi-tenant
    // host) must not grow the cache unboundedly. Clear-on-overflow —
    // the hot set repopulates immediately. Generous: real hosts have
    // a few dozen uids.
    constexpr int kCacheMaxItems = 4096;
    {
        QReadLocker rl(&lock);
        if (const auto it = cache.constFind(uid); it != cache.constEnd())
            return it.value();
    }

    QString name;
    long bufLen = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufLen <= 0) bufLen = 16384;
    QByteArray buf(static_cast<int>(bufLen), Qt::Uninitialized);
    struct passwd pwd{};
    struct passwd *result = nullptr;
    const int rc = ::getpwuid_r(static_cast<uid_t>(uid), &pwd,
                                buf.data(), static_cast<size_t>(buf.size()),
                                &result);
    if (rc == 0 && result && result->pw_name)
        name = QString::fromLocal8Bit(result->pw_name);

    QWriteLocker wl(&lock);
    if (cache.size() >= kCacheMaxItems) cache.clear();
    cache.insert(uid, name);   // cache empty results too (avoids re-probing)
    return name;
#else
    Q_UNUSED(uid);
    return {};
#endif
}

} // namespace qiftop::platform
