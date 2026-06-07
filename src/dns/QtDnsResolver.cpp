#include "QtDnsResolver.h"

#include <QHostInfo>

QtDnsResolver::QtDnsResolver(QObject *parent)
    : DnsResolver(parent)
{}

QString QtDnsResolver::cachedName(const QHostAddress &addr) const
{
    return m_cache.value(addr);
}

void QtDnsResolver::resolve(const QHostAddress &addr)
{
    if (addr.isNull())
        return;
    if (m_cache.contains(addr) || m_pendingAddrs.contains(addr))
        return;

    m_pendingAddrs.insert(addr);
    const int id = QHostInfo::lookupHost(addr.toString(), this,
                                         &QtDnsResolver::onLookupFinished);
    m_pendingById.insert(id, addr);
}

void QtDnsResolver::clearCache()
{
    m_cache.clear();
}

void QtDnsResolver::onLookupFinished(const QHostInfo &info)
{
    const auto it = m_pendingById.constFind(info.lookupId());
    if (it == m_pendingById.constEnd())
        return;
    const QHostAddress addr = *it;
    m_pendingById.erase(it);
    m_pendingAddrs.remove(addr);

    QString name = info.hostName();
    if (info.error() != QHostInfo::NoError || name.isEmpty() || name == addr.toString())
        name = addr.toString(); // negative cache as raw address

    m_cache.insert(addr, name);
    emit resolved(addr, name);
}
