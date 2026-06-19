#include "SettingsDialog.h"
#include "GuiTheme.h"
#include "config/Settings.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {
// Small helper: build a page whose content is a single QFormLayout
// (the common shape of every Preferences page here). Returns the new
// page widget and outputs the form layout to fill.
QWidget *makeFormTab(QFormLayout *&form)
{
    auto *page = new QWidget;
    form = new QFormLayout(page);
    form->setLabelAlignment(Qt::AlignLeft);
    return page;
}

// Pick black or white text for legibility on `bg`, using the WCAG relative
// luminance + the 0.179 crossover (the point where black and white text have
// equal contrast ratio against a colour). More reliable than HSL lightness for
// muddy mid-tones, so the hex label always stands out against its swatch.
QColor contrastText(const QColor &bg)
{
    const auto lin = [](int c) {
        const double s = c / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    const double luminance = 0.2126 * lin(bg.red())
                           + 0.7152 * lin(bg.green())
                           + 0.0722 * lin(bg.blue());
    return luminance > 0.179 ? QColor(Qt::black) : QColor(Qt::white);
}
} // namespace

SettingsDialog::SettingsDialog(Settings *settings,
                               const QStringList &backendCapabilities,
                               QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_backendCaps(backendCapabilities)
{
    setWindowTitle(tr("Preferences"));
    setModal(true);

    // Category navigation on the left (KiCad / VSCode style), pages in a
    // stack on the right. addNavPage() keeps list index == stack index.
    m_navList = new QListWidget;
    m_navList->setObjectName(QStringLiteral("settingsNavList"));
    m_navList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navList->setMaximumWidth(180);
    m_navList->setMinimumWidth(130);
    m_stack = new QStackedWidget;
    const auto addNavPage = [this](QWidget *page, const QString &title) {
        m_stack->addWidget(page);
        m_navList->addItem(title);
    };
    connect(m_navList, &QListWidget::currentRowChanged,
            m_stack, &QStackedWidget::setCurrentIndex);

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

    addNavPage(monitorTab, tr("Monitoring"));

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

    // --- Process & Container Attribution (display section) ----------------
    // Gated by the ACTIVE backend's *-attribution-wire capability tokens
    // (transport-neutral): an old agent, or the in-process Linux conntrack
    // fallback (no resolver), advertises none and the toggles disable with an
    // explanatory tooltip — but the in-process BSD backend DOES attribute, so
    // it lights these up just like the agent. The values still persist so they
    // take effect when the user later runs against an attribution-capable
    // backend.
    const bool hasProcessWire   = m_backendCaps.contains(
        QStringLiteral("process-attribution-wire"));
    const bool hasContainerWire = m_backendCaps.contains(
        QStringLiteral("container-attribution-wire"));
    const bool hasChainWire     = m_backendCaps.contains(
        QStringLiteral("container-chain-wire"));

    auto *sep2 = new QFrame;
    sep2->setFrameShape(QFrame::HLine);
    sep2->setFrameShadow(QFrame::Sunken);
    displayForm->addRow(sep2);

    auto *attribHeader = new QLabel(tr("<b>Process &amp; Container Attribution</b>"));
    displayForm->addRow(attribHeader);

    const QString offTip = tr(
        "The active backend does not advertise this capability — "
        "the column would be empty. The setting still persists and "
        "will take effect when the backend supports it.");

    m_showProcessColumnBox = new QCheckBox(tr("Show Process column"));
    m_showProcessColumnBox->setChecked(m_settings->showProcessColumn());
    if (!hasProcessWire) {
        m_showProcessColumnBox->setEnabled(false);
        m_showProcessColumnBox->setToolTip(offTip);
    } else {
        m_showProcessColumnBox->setToolTip(tr(
            "Reveal the per-flow owning process (comm + pid). "
            "Toggleable any time via the column header's right-click menu."));
    }
    displayForm->addRow(m_showProcessColumnBox);

    m_showContainerColumnBox = new QCheckBox(tr("Show Container column"));
    m_showContainerColumnBox->setChecked(m_settings->showContainerColumn());
    if (!hasContainerWire) {
        m_showContainerColumnBox->setEnabled(false);
        m_showContainerColumnBox->setToolTip(offTip);
    } else {
        m_showContainerColumnBox->setToolTip(tr(
            "Reveal the container runtime + name (or \"(host)\" "
            "for non-containerised flows). A small \"▸ N×\" badge "
            "indicates nested containers."));
    }
    displayForm->addRow(m_showContainerColumnBox);

    m_showChainInTooltipBox = new QCheckBox(tr("Show full container chain in tooltip"));
    m_showChainInTooltipBox->setChecked(m_settings->showContainerChainInTooltip());
    if (!hasChainWire) {
        m_showChainInTooltipBox->setEnabled(false);
        m_showChainInTooltipBox->setToolTip(offTip);
    } else {
        m_showChainInTooltipBox->setToolTip(tr(
            "When a container is nested (e.g. pod → sidecar), the "
            "Container tooltip lists each level from outermost to "
            "innermost. Disable for a one-line summary only."));
    }
    displayForm->addRow(m_showChainInTooltipBox);

    m_showGroupHeaderDetailsBox =
        new QCheckBox(tr("Show extra info on grouping header rows"));
    m_showGroupHeaderDetailsBox->setChecked(m_settings->showGroupHeaderDetails());
    m_showGroupHeaderDetailsBox->setToolTip(tr(
        "When grouping the Connections view by Process or Container, "
        "show extra attribution detail inline on each group header — "
        "the owning user (for Process) or the short container id (for "
        "Container) — plus a full breakdown on hover. Applies only in "
        "the grouped view modes."));
    displayForm->addRow(m_showGroupHeaderDetailsBox);

    m_sortWithinGroupsBox =
        new QCheckBox(tr("Sort within groups (keep group order fixed)"));
    m_sortWithinGroupsBox->setChecked(m_settings->sortWithinGroups());
    m_sortWithinGroupsBox->setToolTip(tr(
        "When the Connections view is grouped, a column-header click sorts "
        "the rows inside each group while leaving the groups themselves in "
        "place. Uncheck to use the classic behaviour, where a header click "
        "also reorders the groups by their aggregated value. Applies only in "
        "the grouped view modes."));
    displayForm->addRow(m_sortWithinGroupsBox);

    addNavPage(displayTab, tr("Display"));

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

    addNavPage(dnsTab, tr("DNS"));

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

    addNavPage(trayTab, tr("Tray"));

    // --- Colors tab (Appearance theme + group-header chip palette) ---
    QFormLayout *colorsForm = nullptr;
    QWidget     *colorsTab  = makeFormTab(colorsForm);

    auto *appearanceHeader = new QLabel(tr(
        "<b>Appearance</b><br>"
        "<span style=\"color:gray;\">Application colour theme. "
        "\"System\" leaves the native Qt palette untouched; the named "
        "themes force the Fusion style so their palette applies "
        "everywhere.</span>"));
    appearanceHeader->setWordWrap(true);
    colorsForm->addRow(appearanceHeader);

    m_themeCombo = new QComboBox;
    for (const auto &t : qiftop::ui::builtinGuiThemes())
        m_themeCombo->addItem(t.name);
    {
        const auto themes = qiftop::ui::builtinGuiThemes();
        int idx = qiftop::ui::guiThemeIndexByName(themes, m_settings->guiThemeName());
        idx = std::max(idx, 0);
        m_themeCombo->setCurrentIndex(idx);
    }
    m_themeCombo->setToolTip(tr(
        "Colour theme for the qiftop window. Applies immediately; the "
        "Connections \"Flow\" column's source/destination accents follow "
        "the chosen theme."));
    colorsForm->addRow(tr("Theme:"), m_themeCombo);

    auto *colorsSep = new QFrame;
    colorsSep->setFrameShape(QFrame::HLine);
    colorsSep->setFrameShadow(QFrame::Sunken);
    colorsForm->addRow(colorsSep);

    auto *colorsHeader = new QLabel(tr(
        "<b>Grouping header colours</b><br>"
        "<span style=\"color:gray;\">Colours for the process / container "
        "info shown on group header rows. Deliberately separate from the "
        "peer (source/destination) colours used in the Flow column.</span>"));
    colorsHeader->setWordWrap(true);
    colorsForm->addRow(colorsHeader);

    m_chipPrimary = QColor(m_settings->chipColorPrimary());
    m_chipUser    = QColor(m_settings->chipColorUser());
    m_chipId      = QColor(m_settings->chipColorId());
    m_chipDetail  = QColor(m_settings->chipColorDetail());

    // Builds a swatch button bound to a working colour member. Clicking
    // opens a colour picker; the swatch repaints to the chosen colour.
    const auto makeSwatch = [this](QColor *working) {
        auto *btn = new QPushButton;
        btn->setFixedWidth(120);
        const auto refresh = [btn, working] {
            btn->setText(working->name());
            const QColor fg = contrastText(*working);
            // The border + padding are load-bearing, not decoration: a
            // QPushButton with a background-color-only stylesheet falls back to
            // the native style and never paints the fill (that's why these
            // swatches used to render flat grey). Specifying the full box model
            // makes Qt honour the background. Monospace so the hex lines up.
            btn->setStyleSheet(QStringLiteral(
                "QPushButton {"
                " background-color:%1; color:%2;"
                " border:1px solid rgba(0,0,0,0.4);"
                " border-radius:4px; padding:5px 10px;"
                " font-family:monospace; }"
                "QPushButton:hover { border:1px solid %2; }")
                .arg(working->name(), fg.name()));
        };
        refresh();
        connect(btn, &QPushButton::clicked, this, [this, working, refresh] {
            const QColor c = QColorDialog::getColor(
                *working, this, tr("Choose colour"));
            if (c.isValid()) { *working = c; refresh(); }
        });
        // Stash the refresh so Reset can repaint every swatch.
        m_chipSwatchRefreshers.append(refresh);
        return btn;
    };

    m_chipPrimaryBtn = makeSwatch(&m_chipPrimary);
    colorsForm->addRow(tr("Process / Container / Interface:"), m_chipPrimaryBtn);
    m_chipUserBtn = makeSwatch(&m_chipUser);
    colorsForm->addRow(tr("User:"), m_chipUserBtn);
    m_chipIdBtn = makeSwatch(&m_chipId);
    colorsForm->addRow(tr("Container ID:"), m_chipIdBtn);
    m_chipDetailBtn = makeSwatch(&m_chipDetail);
    colorsForm->addRow(tr("PID / cmdline / count:"), m_chipDetailBtn);

    auto *resetChips = new QPushButton(tr("Reset to defaults"));
    connect(resetChips, &QPushButton::clicked, this, [this] {
        m_chipPrimary = Settings::defaultChipColorPrimary();
        m_chipUser    = Settings::defaultChipColorUser();
        m_chipId      = Settings::defaultChipColorId();
        m_chipDetail  = Settings::defaultChipColorDetail();
        for (const auto &r : std::as_const(m_chipSwatchRefreshers)) r();
    });
    colorsForm->addRow(QString(), resetChips);

    addNavPage(colorsTab, tr("Colors"));

    // --- Buttons ---
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &SettingsDialog::apply);

    auto *root = new QVBoxLayout(this);
    auto *split = new QHBoxLayout;
    split->addWidget(m_navList);
    split->addWidget(m_stack, /*stretch*/ 1);
    root->addLayout(split, /*stretch*/ 1);
    root->addWidget(buttons);

    // Start on the first category.
    m_navList->setCurrentRow(0);
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
    m_settings->setShowProcessColumn(m_showProcessColumnBox->isChecked());
    m_settings->setShowContainerColumn(m_showContainerColumnBox->isChecked());
    m_settings->setShowContainerChainInTooltip(m_showChainInTooltipBox->isChecked());
    m_settings->setShowGroupHeaderDetails(m_showGroupHeaderDetailsBox->isChecked());
    m_settings->setSortWithinGroups(m_sortWithinGroupsBox->isChecked());
    m_settings->setChipColorPrimary(m_chipPrimary.name());
    m_settings->setChipColorUser(m_chipUser.name());
    m_settings->setChipColorId(m_chipId.name());
    m_settings->setChipColorDetail(m_chipDetail.name());
    if (m_themeCombo)
        m_settings->setGuiThemeName(m_themeCombo->currentText());
    m_settings->setCloseToTray(m_closeToTrayBox->isChecked());
    m_settings->setStartOnLogin(m_startOnLoginBox->isChecked());
}
