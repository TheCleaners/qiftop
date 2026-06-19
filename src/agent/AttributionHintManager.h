#pragma once

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>

#include "backend/ProcessResolver.h"

class QTimer;

namespace qiftop::agent {

// Per-client runtime override for attribution eagerness, modelled on
// IdleManager (the operational sibling that handles polling cadence).
//
// The agent's *config* picks a default attribution eagerness
// (`[attribution] eagerness` → off / balanced / eager). Clients can ask
// for a different mode at runtime via
// `Connections.SetDesiredAttributionEagerness(s)`; each request becomes a
// TTL'd hint keyed by the caller's unique bus name. The EFFECTIVE mode is
// then computed across all live hints:
//
//   1. If the CONFIG mode is `Off`, the effective mode is ALWAYS `Off` —
//      a runtime hint can never re-enable attribution that config turned
//      off. This is the kill-switch guarantee (config `off` builds a
//      NullResolver at startup, so there's nothing to re-tune anyway).
//   2. Otherwise, if at least one live (non-expired) hint exists, the
//      effective mode is the MOST EAGER of those hints
//      (`Eager > Balanced > Off`). The config default does NOT participate
//      here, so a lone client CAN lower a config `eager` to `balanced`.
//   3. Otherwise (no live hints), the effective mode is the config default.
//
// Why most-eager-wins: it stops a low-priority background client from
// disabling the attribution another active client needs, while still
// letting any client temporarily turn things UP. A client wanting global
// `off` gets it only when no one else is asking for more.
//
// Robustness mirrors IdleManager exactly: monotonic `QElapsedTimer` TTL
// (immune to wall-clock jumps), a 64-sender cap that REJECTS (never
// evicts) when full, and a `NameOwnerChanged` subscription that drops a
// peer's hint the moment it disconnects. A periodic prune timer (~half
// TTL) makes an expiring hint that LOWERS the effective mode actually fire
// effectiveModeChanged() without waiting for the next inbound call.
//
// The pure-logic core (the hint table + effectiveMode computation) is
// unit-testable without a bus — see tests/test_attribution_hints.cpp.
class AttributionHintManager : public QObject {
    Q_OBJECT

public:
    using Eagerness = backend::AttributionEagerness;

    // `configMode` is the agent's startup default (from
    // loadAttributionConfig). `hintTtlMs` should match the cadence hint
    // TTL (IdleManager::Config::hintTtlMs, default 10 s) so clients can
    // reuse a single ~half-TTL heartbeat for both.
    explicit AttributionHintManager(Eagerness configMode,
                                    int hintTtlMs = 10'000,
                                    QObject *parent = nullptr);

    [[nodiscard]] Eagerness configMode() const { return m_configMode; }

    // Current effective mode after applying the three rules above. Const:
    // skips (does not erase) expired hints. evaluate() is what actually
    // prunes + emits.
    [[nodiscard]] Eagerness effectiveMode() const;

    // Convenience for DBus: the lowercase wire spelling of effectiveMode().
    [[nodiscard]] QString effectiveModeString() const
    {
        return backend::eagernessToString(effectiveMode());
    }

    // Subscribe to NameOwnerChanged on `bus` so a peer's hint is dropped
    // immediately on disconnect rather than lingering until TTL. Safe to
    // skip in unit tests; TTL is the belt-and-braces fallback.
    void attachBus(const QDBusConnection &bus);

public slots:
    // Record/replace a runtime hint for `sender` (its unique bus name).
    // Returns true if accepted, false if rejected (empty sender, or the
    // 64-entry table is full and this is a new sender). Callers should
    // treat a rejected hint as "did no work" (don't count it as activity).
    bool setHint(const QString &sender, Eagerness mode);

    // Clear `sender`'s hint (the `default`/empty input case). Returns true
    // if a hint existed and was removed (or there was nothing to do for a
    // valid sender); false only for an empty sender.
    bool clearHint(const QString &sender);

signals:
    // Fires whenever effectiveMode() TRANSITIONS to a new value.
    void effectiveModeChanged(qiftop::backend::AttributionEagerness mode);

private slots:
    void onNameOwnerChanged(const QString &name,
                            const QString &oldOwner,
                            const QString &newOwner);

private:
    // Prune expired hints, recompute the effective mode, and emit
    // effectiveModeChanged() if it differs from the last emitted value.
    void evaluate();
    [[nodiscard]] qint64 nowMs() const { return m_clock.elapsed(); }

    struct Hint { Eagerness mode; qint64 expiresAtMs; };

    Eagerness            m_configMode;
    int                  m_hintTtlMs;
    QTimer              *m_pruneTimer = nullptr;
    QElapsedTimer        m_clock;                 // monotonic, for hint expiry
    Eagerness            m_lastEmitted;           // last effectiveMode we announced
    QHash<QString, Hint> m_hints;                 // sender → hint
};

} // namespace qiftop::agent
