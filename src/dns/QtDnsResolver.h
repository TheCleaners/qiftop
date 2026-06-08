#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QSet>

#include "DnsResolver.h"

// QHostInfo-backed DNS resolver. Uses Qt's built-in async lookup, so no
// threads are needed. Results are cached in-process with two bounds:
//
//   * A hard cap on cache size (kMaxEntries). On overflow we evict the
//     oldest entries by insertion order — rough LRU, good enough for the
//     "long-running monitor on a busy host" workload that motivates this.
//   * A TTL on *negative* cache entries (kNegativeTtlMs). Positive
//     results are kept until evicted by the size cap. Negative results
//     (the hostname-equals-raw-address case) re-resolve after the TTL
//     so transient resolver failures don't poison the cache forever.
class QtDnsResolver : public DnsResolver {
    Q_OBJECT

public:
    explicit QtDnsResolver(QObject *parent = nullptr);

    [[nodiscard]] QString cachedName(const QHostAddress &addr) const override;

public slots:
    void resolve(const QHostAddress &addr) override;
    void clearCache() override;

private slots:
    void onLookupFinished(const class QHostInfo &info);

private:
    struct Entry {
        QString name;
        qint64  ageMs    = 0;   // QElapsedTimer::msecsSinceReference at insert
        bool    negative = false;
    };

    void store(const QHostAddress &addr, const QString &name, bool negative);

    QHash<QHostAddress, Entry>   m_cache;
    QList<QHostAddress>          m_order;        // insertion order, for eviction
    QHash<int, QHostAddress>     m_pendingById;  // lookupId -> addr
    QSet<QHostAddress>           m_pendingAddrs; // de-duplication
    QElapsedTimer                m_clock;
};
