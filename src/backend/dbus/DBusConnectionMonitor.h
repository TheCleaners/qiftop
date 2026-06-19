#pragma once

#include "backend/ConnectionMonitor.h"

#include <QDBusMessage>

namespace qiftop::backend::dbus_client {

class DBusConnectionMonitor : public ConnectionMonitor {
    Q_OBJECT

public:
    explicit DBusConnectionMonitor(bool useSessionBus = false, QObject *parent = nullptr);
    ~DBusConnectionMonitor() override;

    void start() override;
    void stop()  override;
    void setDesiredIntervalMs(int ms) override;
    void requestProcessDetails(qint32 pid) override;

    // Set a runtime attribution-eagerness hint on the agent (capability:
    // attribution-eagerness-hints). `mode` is one of `off`/`balanced`/
    // `eager`, or `default`/empty to clear this client's hint. Fire-and-
    // forget async call; the agent's resulting effective mode arrives via
    // the attributionEagernessChanged() signal (and can be read from the
    // AttributionEagerness property). No-op visual plumbing only — the
    // GUI/TUI controls land in a later PR.
    void setDesiredAttributionEagerness(const QString &mode) override;

private slots:
    void onConnectionsChanged(const QDBusMessage &msg);
    void onAttributionChanged(const QDBusMessage &msg);
    void onPermissionDenied(const QString &detail);
    void onAttributionEagernessChanged(const QString &mode);

private:
    void requestInitialSnapshot();
    void sendDesiredIntervalAsync(int ms);

    bool m_useSessionBus = false;
    bool m_started       = false;
    int  m_desiredMs     = 0;
    QString m_desiredEagerness;   // last requested eagerness (re-asserted on start)
};

} // namespace qiftop::backend::dbus_client
