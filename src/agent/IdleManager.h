#pragma once

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>

class ConnectionMonitor;
class NetworkMonitor;
class QTimer;

namespace qiftop::agent {

// Adaptive idle manager for qiftop-agent.
//
// Tracks the time since the last incoming DBus method call (`noteActivity()`)
// and progressively slows down — and eventually pauses — the underlying
// backend monitors:
//
//   0 .. activeWindowMs        → activeIntervalMs        (full speed)
//   activeWindowMs .. slow1Ms  → slow1IntervalMs         (e.g. every 2 s)
//   slow1Ms        .. slow2Ms  → slow2IntervalMs         (e.g. every 5 s)
//   >= idleTimeoutMs           → 0 (paused, no signals)
//
// Any incoming method call resets the timer and immediately restores the
// active interval. Process stays alive; only polling is paused.
//
// Emits cadenceChanged() on every effective interval transition so that
// observers (and clients via a DBus mirror) can tell when the agent has
// slowed down, sped up, or paused. Clients use this to notice that their
// own SetDesiredIntervalMs heartbeat has slipped past hintTtlMs (e.g.
// after a UI hang or suspend/resume) without inferring from missing
// StatsChanged signals.
class IdleManager : public QObject {
    Q_OBJECT

public:
    struct Config {
        int activeIntervalMs = 1000;
        int slow1IntervalMs  = 2000;
        int slow2IntervalMs  = 5000;
        int activeWindowMs   = 30'000;
        int slow1WindowMs    = 45'000;
        int slow2WindowMs    = 60'000;
        int idleTimeoutMs    = 60'000;
        int minIntervalMs    = 100;     // hard floor for hinted cadence
        int hintTtlMs        = 10'000;  // client hints expire after this
    };

    IdleManager(NetworkMonitor *net, ConnectionMonitor *conn,
                Config cfg, QObject *parent = nullptr);

    [[nodiscard]] Config config() const { return m_cfg; }
    [[nodiscard]] int    currentIntervalMs() const { return m_currentMs; }

    // Subscribe to NameOwnerChanged on `bus` so hints from peers that
    // disconnect are dropped immediately instead of lingering until TTL
    // expiry. Safe to skip (e.g. in unit tests with no bus); TTL is the
    // belt-and-braces fallback.
    void attachBus(const QDBusConnection &bus);

signals:
    // Emitted whenever applyInterval() actually changes the polling rate.
    // `ms <= 0` means polling is paused.
    void cadenceChanged(int ms);

public slots:
    // Call from every incoming DBus method handler.
    void noteActivity();

    // Record a per-client cadence request. `sender` is the caller's unique
    // bus name (e.g. ":1.42"); ms<=0 clears the hint. Hints expire after
    // Config::hintTtlMs unless re-asserted.
    void setClientHint(const QString &sender, int ms);

private slots:
    void evaluate();
    void onNameOwnerChanged(const QString &name,
                            const QString &oldOwner,
                            const QString &newOwner);

private:
    int  effectiveActiveIntervalMs();
    void applyInterval(int ms);
    [[nodiscard]] qint64 nowMs() const { return m_clock.elapsed(); }

    struct Hint { int ms; qint64 expiresAtMs; };

    NetworkMonitor      *m_net      = nullptr;
    ConnectionMonitor   *m_conn     = nullptr;
    Config               m_cfg;
    QTimer              *m_timer    = nullptr;
    QElapsedTimer        m_since;                 // since last noteActivity
    QElapsedTimer        m_clock;                 // monotonic clock for hint expiry
    int                  m_currentMs = -1;        // last applied interval
    QHash<QString, Hint> m_hints;                 // sender → hint
};

} // namespace qiftop::agent
