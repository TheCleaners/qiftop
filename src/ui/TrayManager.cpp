#include "TrayManager.h"

#include "config/Settings.h"
#include "util/Units.h"

#include <QAction>
#include <QMenu>
#include <QSystemTrayIcon>

TrayManager::TrayManager(Settings *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    m_elapsed.start();

    m_icon = new QSystemTrayIcon(this);
    m_icon->setIcon(QIcon::fromTheme(QStringLiteral("network-transmit-receive"),
                                     QIcon::fromTheme(QStringLiteral("network-wired"))));
    m_icon->setToolTip(tr("qiftop — waiting for network statistics…"));

    m_menu = new QMenu;
    m_showAction  = m_menu->addAction(tr("&Show window"));
    m_pauseAction = m_menu->addAction(tr("&Pause"));
    m_pauseAction->setCheckable(true);
    m_menu->addSeparator();
    m_quitAction  = m_menu->addAction(tr("&Quit"));
    m_icon->setContextMenu(m_menu);

    connect(m_showAction,  &QAction::triggered, this, &TrayManager::showWindowRequested);
    connect(m_pauseAction, &QAction::toggled,   this, &TrayManager::pauseToggled);
    connect(m_quitAction,  &QAction::triggered, this, &TrayManager::quitRequested);
    connect(m_icon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) { onActivated(int(r)); });

    // Refresh tooltip whenever the user changes which interfaces are summarised.
    connect(m_settings, &Settings::changed, this, [this] {
        if (!m_prev.isEmpty())
            rebuildTooltip({}); // re-render from cached prev only — see note
    });
}

TrayManager::~TrayManager()
{
    delete m_menu;
}

bool TrayManager::isAvailable() const
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayManager::setVisible(bool visible)
{
    m_icon->setVisible(visible && isAvailable());
}

void TrayManager::setPaused(bool paused)
{
    QSignalBlocker block(m_pauseAction);
    m_pauseAction->setChecked(paused);
    m_pauseAction->setText(paused ? tr("&Resume") : tr("&Pause"));
}

void TrayManager::onActivated(int reason)
{
    switch (static_cast<QSystemTrayIcon::ActivationReason>(reason)) {
    case QSystemTrayIcon::Trigger:
    case QSystemTrayIcon::DoubleClick:
        emit showWindowRequested();
        break;
    default:
        break;
    }
}

void TrayManager::onStatsUpdated(const QList<InterfaceStats> &stats)
{
    rebuildTooltip(stats);
}

void TrayManager::rebuildTooltip(const QList<InterfaceStats> &stats)
{
    const qint64 nowMs   = m_elapsed.elapsed();
    const qint64 deltaMs = nowMs - m_lastElapsedMs;
    const double dt      = deltaMs > 0 ? deltaMs / 1000.0 : 1.0;

    // For each interface compute rate from previous sample, while excluding
    // loopbacks from the aggregate totals (consistent with the status bar).
    double totalRx = 0.0, totalTx = 0.0;
    QStringList perIfLines;
    const QStringList selected = m_settings->trayInterfaces();

    for (const InterfaceStats &s : stats) {
        double rx = 0.0, tx = 0.0;
        if (auto it = m_prev.constFind(s.name); it != m_prev.constEnd()) {
            rx = static_cast<double>(s.rxBytes - it->rxBytes) / dt;
            tx = static_cast<double>(s.txBytes - it->txBytes) / dt;
        }
        if (!s.isLoopback) {
            totalRx += rx;
            totalTx += tx;
        }
        if (selected.contains(s.name)) {
            perIfLines << QStringLiteral("%1: ↓ %2 ↑ %3")
                              .arg(s.name,
                                   util::formatByteRate(rx),
                                   util::formatByteRate(tx));
        }
    }

    // Update prev cache + timestamp only when we actually got a new snapshot.
    if (!stats.isEmpty()) {
        m_prev.clear();
        for (const InterfaceStats &s : stats)
            m_prev.insert(s.name, s);
        m_lastElapsedMs = nowMs;
    }

    QStringList lines;
    lines << tr("Total: ↓ %1   ↑ %2")
                 .arg(util::formatByteRate(totalRx),
                      util::formatByteRate(totalTx));
    if (!perIfLines.isEmpty()) {
        lines << QString(); // blank separator
        lines << perIfLines;
    }
    m_icon->setToolTip(lines.join(QLatin1Char('\n')));
}
