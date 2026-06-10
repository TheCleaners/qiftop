#pragma once

#include <QDialog>

class Settings;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

// Modal preferences dialog. Edits a working copy and writes to Settings only
// when the user accepts.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(Settings *settings,
                            const QStringList &agentCapabilities = {},
                            QWidget *parent = nullptr);

private slots:
    void apply();

private:
    Settings *m_settings;
    QStringList m_agentCaps;

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
};
