#include "SettingsDialog.h"
#include "config/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace {
// Small helper: build a tab page whose content is a single QFormLayout
// (the common shape of every Preferences tab here). Returns the new
// page widget and outputs the form layout to fill.
QWidget *makeFormTab(QFormLayout *&form)
{
    auto *page = new QWidget;
    form = new QFormLayout(page);
    form->setLabelAlignment(Qt::AlignLeft);
    return page;
}
} // namespace

SettingsDialog::SettingsDialog(Settings *settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(tr("Preferences"));
    setModal(true);

    auto *tabs = new QTabWidget(this);

    // --- Monitoring tab ---
    QFormLayout *monitorForm = nullptr;
    QWidget     *monitorTab  = makeFormTab(monitorForm);

    m_pollIntervalSpin = new QSpinBox;
    m_pollIntervalSpin->setRange(100, 60'000);
    m_pollIntervalSpin->setSingleStep(250);
    m_pollIntervalSpin->setSuffix(tr(" ms"));
    m_pollIntervalSpin->setValue(m_settings->pollIntervalMs());
    monitorForm->addRow(tr("Poll interval:"), m_pollIntervalSpin);

    m_staleRetentionSpin = new QSpinBox;
    m_staleRetentionSpin->setRange(0, 600);
    m_staleRetentionSpin->setSingleStep(5);
    m_staleRetentionSpin->setSuffix(tr(" s"));
    m_staleRetentionSpin->setSpecialValueText(tr("Off"));
    m_staleRetentionSpin->setToolTip(tr(
        "How long closed connections linger in the table (italic/greyed) "
        "before being pruned. Set to 0 to remove them as soon as they "
        "disappear from the kernel's conntrack table."));
    m_staleRetentionSpin->setValue(m_settings->connectionStaleRetentionSecs());
    monitorForm->addRow(tr("Keep closed connections for:"), m_staleRetentionSpin);

    m_staleRetentionUdpSpin = new QSpinBox;
    m_staleRetentionUdpSpin->setRange(0, 600);
    m_staleRetentionUdpSpin->setSingleStep(10);
    m_staleRetentionUdpSpin->setSuffix(tr(" s"));
    m_staleRetentionUdpSpin->setSpecialValueText(tr("Off"));
    m_staleRetentionUdpSpin->setToolTip(tr(
        "Same as above, but for UDP flows specifically. UDP flows are "
        "bursty (one-shot DNS lookups, etc.) and the kernel times their "
        "conntrack entries out aggressively, so a larger value here "
        "usually feels right."));
    m_staleRetentionUdpSpin->setValue(m_settings->connectionStaleRetentionSecsUdp());
    monitorForm->addRow(tr("Keep closed UDP connections for:"), m_staleRetentionUdpSpin);

    m_udpAggregateBox = new QCheckBox(tr("Aggregate UDP flows by peer"));
    m_udpAggregateBox->setToolTip(tr(
        "When enabled, multiple UDP flows sharing the same peer (e.g. "
        "successive DNS queries to 8.8.8.8:53) are coalesced into a "
        "single row whose ephemeral port shows as \"*\". Keeps the table "
        "readable on systems that spawn many short-lived UDP exchanges."));
    m_udpAggregateBox->setChecked(m_settings->udpAggregateByPeer());
    monitorForm->addRow(m_udpAggregateBox);

    tabs->addTab(monitorTab, tr("Monitoring"));

    // --- Display tab ---
    QFormLayout *displayForm = nullptr;
    QWidget     *displayTab  = makeFormTab(displayForm);

    m_showLoopbackBox = new QCheckBox(tr("Show loopback interfaces"));
    m_showLoopbackBox->setChecked(m_settings->showLoopback());
    displayForm->addRow(m_showLoopbackBox);

    m_showDownBox = new QCheckBox(tr("Show down interfaces"));
    m_showDownBox->setChecked(m_settings->showDown());
    displayForm->addRow(m_showDownBox);

    m_ipv6Box = new QCheckBox(tr("Show IPv6 connections"));
    m_ipv6Box->setChecked(m_settings->ipv6Enabled());
    displayForm->addRow(m_ipv6Box);

    m_showTcpBox = new QCheckBox(tr("Show TCP connections"));
    m_showTcpBox->setChecked(m_settings->showTcp());
    displayForm->addRow(m_showTcpBox);

    m_showUdpBox = new QCheckBox(tr("Show UDP connections"));
    m_showUdpBox->setChecked(m_settings->showUdp());
    displayForm->addRow(m_showUdpBox);

    m_colorCodeBox = new QCheckBox(tr("Color-code connection components"));
    m_colorCodeBox->setToolTip(tr(
        "Tint the parts of each connection line: the source endpoint in a "
        "cool color, the destination in a warm color. The proto tag stays "
        "muted regardless. Turn off for a calmer single-color look."));
    m_colorCodeBox->setChecked(m_settings->colorCodeConnectionFlow());
    displayForm->addRow(m_colorCodeBox);

    m_tintRowBox = new QCheckBox(tr("Tint whole row by direction (green=outbound, red=inbound)"));
    m_tintRowBox->setToolTip(tr(
        "When color-coding is enabled, paint a faint green background on "
        "outbound rows and a faint red background on inbound rows "
        "(Wireshark-style). Rows whose direction can't be inferred get "
        "no tint."));
    m_tintRowBox->setChecked(m_settings->tintRowByDirection());
    // Indent slightly so the dependency on color-coding reads visually.
    displayForm->addRow(QString(), m_tintRowBox);
    // Live enable/disable: tint option is meaningless without color-coding.
    const auto syncTintEnabled = [this] {
        m_tintRowBox->setEnabled(m_colorCodeBox->isChecked());
    };
    connect(m_colorCodeBox, &QCheckBox::toggled, this, syncTintEnabled);
    syncTintEnabled();

    // --- Adaptive throughput gauge (display section) ----------------------
    auto *sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    displayForm->addRow(sep);

    m_throughputGaugeBox = new QCheckBox(tr("Show adaptive throughput gauge"));
    m_throughputGaugeBox->setToolTip(tr(
        "Tracks each connection's peak (or average) rx+tx rate over time "
        "and paints a darker portion of the row's background tint "
        "proportional to its current rate. Also adds Max RX / Max TX "
        "columns showing the per-connection reference value."));
    m_throughputGaugeBox->setChecked(m_settings->throughputGaugeEnabled());
    displayForm->addRow(m_throughputGaugeBox);

    m_throughputModeCombo = new QComboBox;
    m_throughputModeCombo->addItem(tr("Sliding window (max)"),
        int(Settings::ThroughputMaxMode::Windowed));
    m_throughputModeCombo->addItem(tr("Cumulative moving average"),
        int(Settings::ThroughputMaxMode::CumulativeAverage));
    {
        const int idx = m_throughputModeCombo->findData(
            int(m_settings->throughputMaxMode()));
        if (idx >= 0) m_throughputModeCombo->setCurrentIndex(idx);
    }
    m_throughputModeCombo->setToolTip(tr(
        "How the gauge's per-connection reference value is computed.\n"
        "Sliding window: max observed rate over the last N seconds — "
        "responsive but volatile.\n"
        "Cumulative moving average: lifetime average rate — smoother but "
        "drifts toward the connection's long-run typical throughput."));
    displayForm->addRow(tr("Throughput reference:"), m_throughputModeCombo);

    m_throughputWindowSpin = new QSpinBox;
    m_throughputWindowSpin->setRange(2, 3600);
    m_throughputWindowSpin->setSingleStep(5);
    m_throughputWindowSpin->setSuffix(tr(" s"));
    m_throughputWindowSpin->setValue(m_settings->throughputWindowSecs());
    m_throughputWindowSpin->setToolTip(tr(
        "Sliding-window length used by the gauge in window mode. Only "
        "meaningful when the throughput reference is set to \"Sliding "
        "window\"."));
    displayForm->addRow(tr("Window length:"), m_throughputWindowSpin);

    m_rateSmoothingSpin = new QDoubleSpinBox;
    m_rateSmoothingSpin->setRange(0.0, 60.0);
    m_rateSmoothingSpin->setDecimals(2);
    m_rateSmoothingSpin->setSingleStep(0.05);
    m_rateSmoothingSpin->setSuffix(tr(" s"));
    m_rateSmoothingSpin->setSpecialValueText(tr("Off"));
    m_rateSmoothingSpin->setValue(m_settings->rateSmoothingMs() / 1000.0);
    m_rateSmoothingSpin->setToolTip(tr(
        "Exponentially smooth each connection's instantaneous rx/tx "
        "rate. The value is the EMA time constant (in seconds; "
        "sub-second values are supported, e.g. 0.25 s). Roughly, how "
        "many seconds of history dominate the displayed rate. 0 "
        "disables smoothing (raw per-tick deltas). The throughput "
        "gauge and Max RX/TX columns also see the smoothed values."));
    displayForm->addRow(tr("Smooth rates over:"), m_rateSmoothingSpin);

    m_showStatusInTitleBox = new QCheckBox(tr("Show filter summary in window title"));
    m_showStatusInTitleBox->setToolTip(tr(
        "Suffix the application window title with a compact summary "
        "of the active filters: poll interval, comma-separated iface "
        "filter (when not \"all\"), and protocol/family overrides."));
    m_showStatusInTitleBox->setChecked(m_settings->showStatusInTitle());
    displayForm->addRow(m_showStatusInTitleBox);

    // Live enable/disable: the mode/window controls only apply when the
    // gauge is enabled, and window length only applies in Windowed mode.
    const auto syncThroughputEnabled = [this] {
        const bool on = m_throughputGaugeBox->isChecked();
        m_throughputModeCombo->setEnabled(on);
        const bool windowed = on && (m_throughputModeCombo->currentData().toInt()
                                     == int(Settings::ThroughputMaxMode::Windowed));
        m_throughputWindowSpin->setEnabled(windowed);
    };
    connect(m_throughputGaugeBox, &QCheckBox::toggled,
            this, syncThroughputEnabled);
    connect(m_throughputModeCombo, &QComboBox::currentIndexChanged,
            this, syncThroughputEnabled);
    syncThroughputEnabled();

    tabs->addTab(displayTab, tr("Display"));

    // --- DNS tab ---
    QFormLayout *dnsForm = nullptr;
    QWidget     *dnsTab  = makeFormTab(dnsForm);

    m_dnsBox = new QCheckBox(tr("Resolve hostnames (asynchronous)"));
    m_dnsBox->setChecked(m_settings->resolveHostnames());
    dnsForm->addRow(m_dnsBox);

    m_aliasIfaceBox = new QCheckBox(tr("Render own interface addresses as \"localhost\""));
    m_aliasIfaceBox->setToolTip(tr(
        "When DNS resolution is on, this host's own non-loopback "
        "interface addresses (e.g. 192.168.1.42) are shown as "
        "\"localhost\". Disable to keep them as their numeric IP. "
        "Loopback addresses (127.0.0.1, ::1) are always shown as "
        "\"localhost\"."));
    m_aliasIfaceBox->setChecked(m_settings->resolveIfaceAddrsAsLocalhost());
    dnsForm->addRow(m_aliasIfaceBox);

    tabs->addTab(dnsTab, tr("DNS"));

    // --- Tray tab ---
    QFormLayout *trayForm = nullptr;
    QWidget     *trayTab  = makeFormTab(trayForm);

    m_closeToTrayBox = new QCheckBox(tr("Close to tray instead of exiting"));
    m_closeToTrayBox->setChecked(m_settings->closeToTray());
    trayForm->addRow(m_closeToTrayBox);

    m_startOnLoginBox = new QCheckBox(tr("Start on login (silently into tray)"));
    m_startOnLoginBox->setChecked(m_settings->startOnLogin());
    m_startOnLoginBox->setToolTip(tr(
        "Installs an XDG autostart entry under ~/.config/autostart/. "
        "Launches qiftop with --tray at desktop login so it comes up "
        "in the system tray rather than popping a window."));
    trayForm->addRow(m_startOnLoginBox);

    tabs->addTab(trayTab, tr("Tray"));

    // --- Buttons ---
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &SettingsDialog::apply);

    auto *root = new QVBoxLayout(this);
    root->addWidget(tabs);
    root->addWidget(buttons);
}

void SettingsDialog::apply()
{
    m_settings->setPollIntervalMs(m_pollIntervalSpin->value());
    m_settings->setConnectionStaleRetentionSecs(m_staleRetentionSpin->value());
    m_settings->setConnectionStaleRetentionSecsUdp(m_staleRetentionUdpSpin->value());
    m_settings->setUdpAggregateByPeer(m_udpAggregateBox->isChecked());
    m_settings->setShowLoopback(m_showLoopbackBox->isChecked());
    m_settings->setShowDown(m_showDownBox->isChecked());
    m_settings->setIpv6Enabled(m_ipv6Box->isChecked());
    m_settings->setShowTcp(m_showTcpBox->isChecked());
    m_settings->setShowUdp(m_showUdpBox->isChecked());
    m_settings->setResolveHostnames(m_dnsBox->isChecked());
    m_settings->setResolveIfaceAddrsAsLocalhost(m_aliasIfaceBox->isChecked());
    m_settings->setColorCodeConnectionFlow(m_colorCodeBox->isChecked());
    m_settings->setTintRowByDirection(m_tintRowBox->isChecked());
    m_settings->setThroughputGaugeEnabled(m_throughputGaugeBox->isChecked());
    m_settings->setThroughputMaxMode(
        static_cast<Settings::ThroughputMaxMode>(
            m_throughputModeCombo->currentData().toInt()));
    m_settings->setThroughputWindowSecs(m_throughputWindowSpin->value());
    m_settings->setRateSmoothingMs(int(qRound(m_rateSmoothingSpin->value() * 1000.0)));
    m_settings->setShowStatusInTitle(m_showStatusInTitleBox->isChecked());
    m_settings->setCloseToTray(m_closeToTrayBox->isChecked());
    m_settings->setStartOnLogin(m_startOnLoginBox->isChecked());
}
