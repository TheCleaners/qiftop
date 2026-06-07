#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>

#include "backend/NetworkMonitor.h"

class QAction;
class QMenu;
class QSystemTrayIcon;
class Settings;

// Manages the system tray icon: tooltip with live aggregate + per-interface
// rates (limited to those the user opted in via Settings::trayInterfaces),
// and a context menu (Show, Pause, Quit).
//
// Availability is not guaranteed on all sessions — check isAvailable() before
// relying on tray-only behaviours such as close-to-tray.
class TrayManager : public QObject {
    Q_OBJECT

public:
    TrayManager(Settings *settings, QObject *parent = nullptr);
    ~TrayManager() override;

    [[nodiscard]] bool isAvailable() const;
    void setVisible(bool visible);

    // Reflects the current Pause state to the tray context menu.
    void setPaused(bool paused);

public slots:
    // Consume the same stat snapshots the model receives; computes per-tick
    // rates and refreshes the tooltip.
    void onStatsUpdated(const QList<InterfaceStats> &stats);

signals:
    // Triggered by left-click / DoubleClick / "Show window" menu entry.
    void showWindowRequested();
    // From "Pause"/"Resume" checkable menu entry.
    void pauseToggled(bool paused);
    // From "Quit" menu entry — caller is expected to actually quit the app.
    void quitRequested();

private slots:
    void onActivated(int reason); // QSystemTrayIcon::ActivationReason

private:
    void rebuildTooltip(const QList<InterfaceStats> &stats);

    Settings        *m_settings = nullptr;
    QSystemTrayIcon *m_icon     = nullptr;
    QMenu           *m_menu     = nullptr;
    QAction         *m_showAction  = nullptr;
    QAction         *m_pauseAction = nullptr;
    QAction         *m_quitAction  = nullptr;

    // Previous-sample byte counters per interface, plus a per-source elapsed
    // timer for rate computation independent of the model.
    QHash<QString, InterfaceStats> m_prev;
    QElapsedTimer                  m_elapsed;
    qint64                         m_lastElapsedMs = 0;
};
