#include "PlatformInfo.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#if defined(Q_OS_UNIX)
#  include <ifaddrs.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <grp.h>
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

bool userInGroup(uint uid, const QString &groupName)
{
#if defined(Q_OS_UNIX)
    if (groupName.isEmpty())
        return false;

    // Resolve the user's login name + primary gid.
    long pwLen = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (pwLen <= 0) pwLen = 16384;
    QByteArray pwBuf(static_cast<int>(pwLen), Qt::Uninitialized);
    struct passwd pwd{};
    struct passwd *pwres = nullptr;
    if (::getpwuid_r(static_cast<uid_t>(uid), &pwd, pwBuf.data(),
                     static_cast<size_t>(pwBuf.size()), &pwres) != 0 || !pwres)
        return false;
    const QByteArray loginName(pwres->pw_name ? pwres->pw_name : "");
    const gid_t primaryGid = pwres->pw_gid;

    // Resolve the target group by name.
    long grLen = ::sysconf(_SC_GETGR_R_SIZE_MAX);
    if (grLen <= 0) grLen = 16384;
    QByteArray grBuf(static_cast<int>(grLen), Qt::Uninitialized);
    struct group grp{};
    struct group *grres = nullptr;
    if (::getgrnam_r(groupName.toLocal8Bit().constData(), &grp, grBuf.data(),
                     static_cast<size_t>(grBuf.size()), &grres) != 0 || !grres)
        return false;

    if (grres->gr_gid == primaryGid)
        return true;                          // primary group match
    for (char **m = grres->gr_mem; m && *m; ++m)
        if (loginName == *m)
            return true;                      // supplementary group match
    return false;
#else
    Q_UNUSED(uid);
    Q_UNUSED(groupName);
    return false;
#endif
}

bool settingsWriteWouldEscalate()
{
#if defined(Q_OS_UNIX)
    // Only privileged processes can create files owned by another user.
    if (::geteuid() != 0)
        return false;

    // The directory QSettings(IniFormat user scope) writes into. Both the
    // GUI (org "qiftop", app "qiftop") and the TUI ("nqiftop") land under
    // this GenericConfigLocation root (…/.config), so checking the root's
    // ownership covers every per-user .conf we might write.
    const QString cfgRoot =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (cfgRoot.isEmpty())
        return false;

    // Walk up to the nearest existing ancestor — the leaf (…/.config or the
    // qiftop subdir) may not exist yet on a first run, in which case we'd be
    // about to CREATE it; the owner of the parent ($HOME) is then decisive.
    for (QString p = cfgRoot; !p.isEmpty(); ) {
        struct stat st{};
        if (::stat(p.toLocal8Bit().constData(), &st) == 0)
            return st.st_uid != 0;   // existing tree owned by a non-root user
        const QString parent = QFileInfo(p).path();
        if (parent == p)             // reached "/" without resolving
            break;
        p = parent;
    }
    return false;
#else
    return false;
#endif
}

} // namespace qiftop::platform
