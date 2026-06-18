#pragma once

#include <QColor>
#include <QDialog>
#include <QList>

#include <functional>

class Settings;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;

// Modal preferences dialog. Edits a working copy and writes to Settings only
// when the user accepts.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(Settings *settings,
                            const QStringList &backendCapabilities = {},
                            QWidget *parent = nullptr);

private slots:
    void apply();

private:
    Settings *m_settings;
    // Transport-neutral capability tokens of the ACTIVE backend (agent proxy
    // or in-process). Gates the attribution toggles — no agent-presence axis.
    QStringList m_backendCaps;

    QSpinBox  *m_pollIntervalSpin = nullptr;
    QSpinBox  *m_staleRetentionSpin = nullptr;
    QSpinBox  *m_staleRetentionUdpSpin = nullptr;
    QCheckBox *m_udpAggregateBox  = nullptr;
    QCheckBox *m_showLoopbackBox  = nullptr;
    QCheckBox *m_showDownBox      = nullptr;
    QCheckBox *m_ipv6Box          = nullptr;
    QCheckBox *m_dnsBox           = nullptr;
    QCheckBox *m_aliasIfaceBox    = nullptr;
    QCheckBox *m_colorCodeBox     = nullptr;
    QCheckBox *m_tintRowBox       = nullptr;
    QCheckBox *m_showTcpBox       = nullptr;
    QCheckBox *m_showUdpBox       = nullptr;
    QCheckBox *m_closeToTrayBox   = nullptr;
    QCheckBox *m_startOnLoginBox  = nullptr;
    QCheckBox *m_throughputGaugeBox  = nullptr;
    QComboBox *m_throughputModeCombo = nullptr;
    QSpinBox  *m_throughputWindowSpin = nullptr;
    QDoubleSpinBox *m_rateSmoothingSpin = nullptr;
    QCheckBox *m_showStatusInTitleBox = nullptr;
    QCheckBox *m_showProcessColumnBox   = nullptr;
    QCheckBox *m_showContainerColumnBox = nullptr;
    QCheckBox *m_showChainInTooltipBox  = nullptr;
    QCheckBox *m_showGroupHeaderDetailsBox = nullptr;
    QCheckBox *m_sortWithinGroupsBox = nullptr;

    // Left-hand category navigation (KiCad/VSCode style) → page stack.
    QListWidget    *m_navList = nullptr;
    QStackedWidget *m_stack   = nullptr;

    // Appearance: GUI colour theme selector (Colors tab).
    QComboBox *m_themeCombo = nullptr;

    // Group-header chip palette working copies + their swatch buttons.
    QColor m_chipPrimary, m_chipUser, m_chipId, m_chipDetail;
    QPushButton *m_chipPrimaryBtn = nullptr;
    QPushButton *m_chipUserBtn    = nullptr;
    QPushButton *m_chipIdBtn      = nullptr;
    QPushButton *m_chipDetailBtn  = nullptr;
    QList<std::function<void()>> m_chipSwatchRefreshers;
};
