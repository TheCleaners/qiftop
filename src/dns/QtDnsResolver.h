#pragma once

#include <QHash>
#include <QSet>

#include "DnsResolver.h"

// QHostInfo-backed DNS resolver. Uses Qt's built-in async lookup, so no
// threads are needed. Results (including negatives) are cached in-process.
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
    QHash<QHostAddress, QString> m_cache;        // addr -> hostname (or addr-string on failure)
    QHash<int, QHostAddress>     m_pendingById;  // lookupId -> addr
    QSet<QHostAddress>           m_pendingAddrs; // de-duplication
};
