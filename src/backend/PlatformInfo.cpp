#include "PlatformInfo.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringList>

#if defined(Q_OS_UNIX)
#  include <ifaddrs.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
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

} // namespace qiftop::platform
