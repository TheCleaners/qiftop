#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>

// Abstract async DNS resolver.
//
// Callers invoke resolve() and listen for the resolved() signal. Implementations
// should de-duplicate in-flight requests and cache results (positive + negative).
// cachedName() must be cheap, returning immediately if a result is known.
class DnsResolver : public QObject {
    Q_OBJECT

public:
    explicit DnsResolver(QObject *parent = nullptr);
    ~DnsResolver() override;

    // Returns the cached hostname for addr, or an empty string if no result
    // is cached yet. Does NOT trigger a lookup.
    [[nodiscard]] virtual QString cachedName(const QHostAddress &addr) const = 0;

public slots:
    // Schedule (or de-duplicate) an async lookup for addr.
    virtual void resolve(const QHostAddress &addr) = 0;

    // Drop all cached results. Useful when toggling the feature off/on.
    virtual void clearCache() = 0;

signals:
    // Emitted once per successful or failed lookup. On failure, hostname
    // equals addr.toString() so callers can cache the negative result.
    void resolved(QHostAddress addr, QString hostname);
};
