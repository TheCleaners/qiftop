#include "QtDnsResolver.h"

#include <QHostInfo>

namespace {
// Tunables. Picked so that on a busy router (~thousands of unique peers/day)
// the cache stays in the tens of KB and forgotten peers don't keep us from
// looking up new ones, while a typical workstation never hits the eviction
// path at all.
constexpr int    kMaxEntries     = 4096;
constexpr qint64 kNegativeTtlMs  = qint64(5) * 60 * 1000;  // 5 min
} // namespace

QtDnsResolver::QtDnsResolver(QObject *parent)
    : DnsResolver(parent)
{
    m_clock.start();
}

QString QtDnsResolver::cachedName(const QHostAddress &addr) const
{
    return m_cache.value(addr).name;
}

void QtDnsResolver::resolve(const QHostAddress &addr)
{
    if (addr.isNull())
        return;
    if (m_pendingAddrs.contains(addr))
        return;
    if (const auto it = m_cache.constFind(addr); it != m_cache.constEnd()) {
        // Re-resolve expired negatives; positives are sticky until evicted.
        if (!it->negative) return;
        if (m_clock.msecsSinceReference() - it->ageMs < kNegativeTtlMs) return;
        // Fall through to re-resolve. The old entry will be overwritten
        // (and re-positioned in m_order) when the lookup completes.
    }

    m_pendingAddrs.insert(addr);
    const int id = QHostInfo::lookupHost(addr.toString(), this,
                                         &QtDnsResolver::onLookupFinished);
    m_pendingById.insert(id, addr);
}

void QtDnsResolver::clearCache()
{
    m_cache.clear();
    m_order.clear();
}

void QtDnsResolver::primeCacheForTest(const QHostAddress &addr,
                                      const QString      &name,
                                      bool                negative)
{
    store(addr, name, negative);
}

void QtDnsResolver::store(const QHostAddress &addr, const QString &name, bool negative)
{
    if (!m_cache.contains(addr))
        m_order.append(addr);
    m_cache.insert(addr, Entry{name, m_clock.msecsSinceReference(), negative});

    // Evict oldest-first when over the cap. Drop in batches (~3%) so we
    // amortise the cost across many inserts rather than evicting one at a
    // time per oversize insert.
    if (m_cache.size() > kMaxEntries) {
        const int toDrop = qMax(1, kMaxEntries / 32);
        for (int i = 0; i < toDrop && !m_order.isEmpty(); ++i)
            m_cache.remove(m_order.takeFirst());
    }
}

void QtDnsResolver::onLookupFinished(const QHostInfo &info)
{
    const auto it = m_pendingById.constFind(info.lookupId());
    if (it == m_pendingById.constEnd())
        return;
    // Real copy, not a const ref: erase(it) on the next line invalidates *it,
    // and addr is read several times afterwards.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const QHostAddress addr = *it;
    m_pendingById.erase(it);
    m_pendingAddrs.remove(addr);

    QString name = info.hostName();
    const bool failed = (info.error() != QHostInfo::NoError
                         || name.isEmpty()
                         || name == addr.toString());
    if (failed)
        name = addr.toString(); // negative cache as raw address

    store(addr, name, failed);
    emit resolved(addr, name);
}
