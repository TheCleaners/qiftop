#include "Settings.h"

#include "backend/PlatformInfo.h"
#include "util/Autostart.h"

#include <QRegularExpression>

namespace {
// Validate a #rrggbb / #rgb / #aarrggbb hex colour string WITHOUT
// pulling QtGui (QColor) into config/ — Settings must stay Qt6::Core
// only so the lightweight test targets link cleanly.
bool isHexColor(const QString &s)
{
    static const QRegularExpression re(
        QStringLiteral("^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"));
    return re.match(s).hasMatch();
}
} // namespace

namespace {
constexpr auto kPollIntervalMs   = "monitor/pollIntervalMs";
constexpr auto kShowLoopback     = "display/showLoopback";
constexpr auto kShowDown         = "display/showDown";
constexpr auto kIpv6Enabled      = "network/ipv6Enabled";
constexpr auto kResolveHostnames = "dns/resolveHostnames";
constexpr auto kCloseToTray      = "tray/closeToTray";
constexpr auto kTrayInterfaces   = "tray/interfaces";
constexpr auto kConnVisibleIfaces = "connections/visibleIfaces";
constexpr auto kConnStaleRetentionSecs    = "connections/staleRetentionSecs";
constexpr auto kConnStaleRetentionSecsUdp = "connections/staleRetentionSecsUdp";
constexpr auto kUdpAggregateByPeer        = "connections/udpAggregateByPeer";
constexpr auto kResolveIfaceAsLocalhost   = "dns/resolveIfaceAddrsAsLocalhost";
constexpr auto kColorCodeConnFlow         = "display/colorCodeConnectionFlow";
constexpr auto kTintRowByDirection        = "display/tintRowByDirection";
constexpr auto kShowTcp                   = "display/showTcp";
constexpr auto kShowUdp                   = "display/showUdp";
constexpr auto kThroughputGaugeEnabled    = "display/throughputGaugeEnabled";
constexpr auto kInterfaceGaugeEnabled     = "display/interfaceGaugeEnabled";
constexpr auto kThroughputMaxMode         = "display/throughputMaxMode";
constexpr auto kThroughputWindowSecs      = "display/throughputWindowSecs";
constexpr auto kRateSmoothingMs           = "display/rateSmoothingMs";
constexpr auto kRateSmoothingSecsLegacy   = "display/rateSmoothingSecs"; // pre-2026-06-06
constexpr auto kShowStatusInTitle         = "display/showStatusInTitle";
constexpr auto kConnectionFilterExpr      = "connections/filterExpr";
constexpr auto kGuiThemeName              = "display/guiTheme";
constexpr auto kConnectionViewMode        = "connections/viewMode";
constexpr auto kShowProcessColumn         = "connections/showProcessColumn";
constexpr auto kShowContainerColumn       = "connections/showContainerColumn";
constexpr auto kShowContainerChainInTooltip = "connections/showContainerChainInTooltip";
constexpr auto kShowGroupHeaderDetails      = "connections/showGroupHeaderDetails";
constexpr auto kSortWithinGroups            = "connections/sortWithinGroups";
constexpr auto kChipColorPrimary = "connections/chipColorPrimary";
constexpr auto kChipColorUser    = "connections/chipColorUser";
constexpr auto kChipColorId      = "connections/chipColorId";
constexpr auto kChipColorDetail  = "connections/chipColorDetail";
} // namespace

Settings::Settings(QObject *parent)
    : QObject(parent)
{
    // Refuse to write QSettings into another user's $HOME when running
    // privileged (e.g. `sudo -E nqiftop`, or the GUI self-elevation re-exec
    // which forwards HOME). Otherwise every exit leaves root-owned .conf
    // files in the invoking user's ~/.config. We still LOAD the user's
    // settings below — only persistence is suppressed.
    m_persist = !qiftop::platform::settingsWriteWouldEscalate();
    if (!m_persist) {
        qWarning("Settings: running privileged with a config directory owned "
                 "by another user (%s); settings will be read but NOT written "
                 "to avoid creating root-owned files in their home directory.",
                 qUtf8Printable(m_store.fileName()));
    }
    load();
}

void Settings::load()
{
    m_pollIntervalMs   = m_store.value(kPollIntervalMs,   m_pollIntervalMs).toInt();
    m_showLoopback     = m_store.value(kShowLoopback,     m_showLoopback).toBool();
    m_showDown         = m_store.value(kShowDown,         m_showDown).toBool();
    m_ipv6Enabled      = m_store.value(kIpv6Enabled,      m_ipv6Enabled).toBool();
    m_resolveHostnames = m_store.value(kResolveHostnames, m_resolveHostnames).toBool();
    m_closeToTray      = m_store.value(kCloseToTray,      m_closeToTray).toBool();
    m_trayInterfaces   = m_store.value(kTrayInterfaces,   m_trayInterfaces).toStringList();
    m_connVisibleIfaces = m_store.value(kConnVisibleIfaces, m_connVisibleIfaces).toStringList();
    m_connStaleRetentionSecs    = m_store.value(kConnStaleRetentionSecs,
                                                m_connStaleRetentionSecs).toInt();
    m_connStaleRetentionSecsUdp = m_store.value(kConnStaleRetentionSecsUdp,
                                                m_connStaleRetentionSecsUdp).toInt();
    m_udpAggregateByPeer        = m_store.value(kUdpAggregateByPeer,
                                                m_udpAggregateByPeer).toBool();
    m_resolveIfaceAddrsAsLocalhost = m_store.value(kResolveIfaceAsLocalhost,
                                                m_resolveIfaceAddrsAsLocalhost).toBool();
    m_colorCodeConnectionFlow      = m_store.value(kColorCodeConnFlow,
                                                m_colorCodeConnectionFlow).toBool();
    m_tintRowByDirection           = m_store.value(kTintRowByDirection,
                                                m_tintRowByDirection).toBool();
    m_showTcp                      = m_store.value(kShowTcp, m_showTcp).toBool();
    m_showUdp                      = m_store.value(kShowUdp, m_showUdp).toBool();
    m_throughputGaugeEnabled       = m_store.value(kThroughputGaugeEnabled,
                                                m_throughputGaugeEnabled).toBool();
    m_interfaceGaugeEnabled        = m_store.value(kInterfaceGaugeEnabled,
                                                m_interfaceGaugeEnabled).toBool();
    m_throughputMaxMode            = static_cast<ThroughputMaxMode>(
        m_store.value(kThroughputMaxMode,
                      static_cast<int>(m_throughputMaxMode)).toInt());
    m_throughputWindowSecs         = m_store.value(kThroughputWindowSecs,
                                                m_throughputWindowSecs).toInt();
    m_rateSmoothingMs              = m_store.value(kRateSmoothingMs,
                                                m_rateSmoothingMs).toInt();
    // Migrate legacy whole-second setting if present and the new key
    // is at its default. Keeps prior user prefs visually unchanged
    // (1s previously → 1000ms now) on first run after upgrade.
    if (m_rateSmoothingMs == 0 && m_store.contains(kRateSmoothingSecsLegacy)) {
        const int legacy = m_store.value(kRateSmoothingSecsLegacy).toInt();
        if (legacy > 0) {
            m_rateSmoothingMs = legacy * 1000;
            if (m_persist)
                m_store.setValue(QString::fromLatin1(kRateSmoothingMs),
                                 m_rateSmoothingMs);
        }
        if (m_persist)
            m_store.remove(QString::fromLatin1(kRateSmoothingSecsLegacy));
    }
    m_showStatusInTitle            = m_store.value(kShowStatusInTitle,
                                                m_showStatusInTitle).toBool();
    m_connFilterExpr               = m_store.value(kConnectionFilterExpr,
                                                m_connFilterExpr).toString();
    m_guiThemeName                 = m_store.value(kGuiThemeName,
                                                m_guiThemeName).toString();
    {
        const int mode = m_store.value(kConnectionViewMode,
                                       static_cast<int>(m_connViewMode)).toInt();
        if (mode >= 0 && mode <= static_cast<int>(ConnectionViewMode::ByProcess))
            m_connViewMode = static_cast<ConnectionViewMode>(mode);
    }
    m_showProcessColumn   = m_store.value(kShowProcessColumn,   m_showProcessColumn).toBool();
    m_showContainerColumn = m_store.value(kShowContainerColumn, m_showContainerColumn).toBool();
    m_showContainerChainInTooltip = m_store.value(kShowContainerChainInTooltip,
                                                  m_showContainerChainInTooltip).toBool();
    m_showGroupHeaderDetails = m_store.value(kShowGroupHeaderDetails,
                                             m_showGroupHeaderDetails).toBool();
    m_sortWithinGroups       = m_store.value(kSortWithinGroups,
                                             m_sortWithinGroups).toBool();

    const auto loadColor = [this](const char *key, const QString &def) {
        const QString s = m_store.value(QString::fromLatin1(key), def).toString();
        return isHexColor(s) ? s : def;
    };
    m_chipColorPrimary = loadColor(kChipColorPrimary, defaultChipColorPrimary());
    m_chipColorUser    = loadColor(kChipColorUser,    defaultChipColorUser());
    m_chipColorId      = loadColor(kChipColorId,      defaultChipColorId());
    m_chipColorDetail  = loadColor(kChipColorDetail,  defaultChipColorDetail());
}

void Settings::store(const char *key, const QVariant &value)
{
    if (!m_persist) return;   // privileged-for-another-user: never write
    m_store.setValue(QString::fromLatin1(key), value);
}

void Settings::setPollIntervalMs(int ms)
{
    ms = qBound(100, ms, 60'000);
    if (ms == m_pollIntervalMs) return;
    m_pollIntervalMs = ms;
    store(kPollIntervalMs, ms);
    emit changed();
}

void Settings::setShowLoopback(bool v)
{
    if (v == m_showLoopback) return;
    m_showLoopback = v;
    store(kShowLoopback, v);
    emit changed();
}

void Settings::setShowDown(bool v)
{
    if (v == m_showDown) return;
    m_showDown = v;
    store(kShowDown, v);
    emit changed();
}

void Settings::setIpv6Enabled(bool v)
{
    if (v == m_ipv6Enabled) return;
    m_ipv6Enabled = v;
    store(kIpv6Enabled, v);
    emit changed();
}

void Settings::setResolveHostnames(bool v)
{
    if (v == m_resolveHostnames) return;
    m_resolveHostnames = v;
    store(kResolveHostnames, v);
    emit changed();
}

void Settings::setCloseToTray(bool v)
{
    if (v == m_closeToTray) return;
    m_closeToTray = v;
    store(kCloseToTray, v);
    emit changed();
}

void Settings::setTrayInterfaces(const QStringList &v)
{
    if (v == m_trayInterfaces) return;
    m_trayInterfaces = v;
    store(kTrayInterfaces, v);
    emit changed();
}

void Settings::setConnectionVisibleIfaces(const QStringList &v)
{
    if (v == m_connVisibleIfaces) return;
    m_connVisibleIfaces = v;
    store(kConnVisibleIfaces, v);
    emit changed();
}

void Settings::setConnectionVisibleIfacesTransient(const QStringList &v)
{
    if (v == m_connVisibleIfaces) return;
    m_connVisibleIfaces = v;
    // Intentionally not persisted; see header for rationale.
    emit changed();
}

void Settings::setConnectionStaleRetentionSecs(int secs)
{
    secs = qBound(0, secs, 600);
    if (secs == m_connStaleRetentionSecs) return;
    m_connStaleRetentionSecs = secs;
    store(kConnStaleRetentionSecs, secs);
    emit changed();
}

void Settings::setConnectionStaleRetentionSecsUdp(int secs)
{
    secs = qBound(0, secs, 600);
    if (secs == m_connStaleRetentionSecsUdp) return;
    m_connStaleRetentionSecsUdp = secs;
    store(kConnStaleRetentionSecsUdp, secs);
    emit changed();
}

void Settings::setUdpAggregateByPeer(bool v)
{
    if (v == m_udpAggregateByPeer) return;
    m_udpAggregateByPeer = v;
    store(kUdpAggregateByPeer, v);
    emit changed();
}

void Settings::setResolveIfaceAddrsAsLocalhost(bool v)
{
    if (v == m_resolveIfaceAddrsAsLocalhost) return;
    m_resolveIfaceAddrsAsLocalhost = v;
    store(kResolveIfaceAsLocalhost, v);
    emit changed();
}

void Settings::setColorCodeConnectionFlow(bool v)
{
    if (v == m_colorCodeConnectionFlow) return;
    m_colorCodeConnectionFlow = v;
    store(kColorCodeConnFlow, v);
    emit changed();
}

void Settings::setTintRowByDirection(bool v)
{
    if (v == m_tintRowByDirection) return;
    m_tintRowByDirection = v;
    store(kTintRowByDirection, v);
    emit changed();
}

void Settings::setShowTcp(bool v)
{
    if (v == m_showTcp) return;
    m_showTcp = v;
    store(kShowTcp, v);
    emit changed();
}

void Settings::setShowUdp(bool v)
{
    if (v == m_showUdp) return;
    m_showUdp = v;
    store(kShowUdp, v);
    emit changed();
}

void Settings::setThroughputGaugeEnabled(bool v)
{
    if (v == m_throughputGaugeEnabled) return;
    m_throughputGaugeEnabled = v;
    store(kThroughputGaugeEnabled, v);
    emit changed();
}

void Settings::setInterfaceGaugeEnabled(bool v)
{
    if (v == m_interfaceGaugeEnabled) return;
    m_interfaceGaugeEnabled = v;
    store(kInterfaceGaugeEnabled, v);
    emit changed();
}

void Settings::setThroughputMaxMode(ThroughputMaxMode m)
{
    if (m == m_throughputMaxMode) return;
    m_throughputMaxMode = m;
    store(kThroughputMaxMode, static_cast<int>(m));
    emit changed();
}

void Settings::setThroughputWindowSecs(int secs)
{
    secs = qBound(2, secs, 3600);
    if (secs == m_throughputWindowSecs) return;
    m_throughputWindowSecs = secs;
    store(kThroughputWindowSecs, secs);
    emit changed();
}

void Settings::setRateSmoothingMs(int ms)
{
    ms = qBound(0, ms, 60'000);
    if (ms == m_rateSmoothingMs) return;
    m_rateSmoothingMs = ms;
    store(kRateSmoothingMs, ms);
    emit changed();
}

void Settings::setShowStatusInTitle(bool v)
{
    if (v == m_showStatusInTitle) return;
    m_showStatusInTitle = v;
    store(kShowStatusInTitle, v);
    emit changed();
}

bool Settings::startOnLogin() const
{
    return qiftop::autostart::isEnabled();
}

void Settings::setStartOnLogin(bool v)
{
    // No QSettings persistence: source of truth is the autostart file.
    // Only emit changed() when the on-disk state actually changes.
    const bool before = qiftop::autostart::isEnabled();
    if (before == v) return;
    if (qiftop::autostart::setEnabled(v))
        emit changed();
}

void Settings::setConnectionFilterExpr(const QString &expr)
{
    if (expr == m_connFilterExpr) return;
    m_connFilterExpr = expr;
    store(kConnectionFilterExpr, expr);
    emit changed();
}

void Settings::setGuiThemeName(const QString &name)
{
    if (name == m_guiThemeName) return;
    m_guiThemeName = name;
    store(kGuiThemeName, name);
    emit changed();
}

void Settings::setConnectionViewMode(ConnectionViewMode m)
{
    if (m == m_connViewMode) return;
    m_connViewMode = m;
    store(kConnectionViewMode, static_cast<int>(m));
    emit changed();
}

void Settings::setShowProcessColumn(bool v)
{
    if (v == m_showProcessColumn) return;
    m_showProcessColumn = v;
    store(kShowProcessColumn, v);
    emit changed();
}

void Settings::setShowContainerColumn(bool v)
{
    if (v == m_showContainerColumn) return;
    m_showContainerColumn = v;
    store(kShowContainerColumn, v);
    emit changed();
}

void Settings::setShowContainerChainInTooltip(bool v)
{
    if (v == m_showContainerChainInTooltip) return;
    m_showContainerChainInTooltip = v;
    store(kShowContainerChainInTooltip, v);
    emit changed();
}

void Settings::setShowGroupHeaderDetails(bool v)
{
    if (v == m_showGroupHeaderDetails) return;
    m_showGroupHeaderDetails = v;
    store(kShowGroupHeaderDetails, v);
    emit changed();
}

void Settings::setSortWithinGroups(bool v)
{
    if (v == m_sortWithinGroups) return;
    m_sortWithinGroups = v;
    store(kSortWithinGroups, v);
    emit changed();
}

void Settings::setChipColorPrimary(const QString &hex)
{
    if (!isHexColor(hex) || hex == m_chipColorPrimary) return;
    m_chipColorPrimary = hex;
    store(kChipColorPrimary, hex);
    emit changed();
}

void Settings::setChipColorUser(const QString &hex)
{
    if (!isHexColor(hex) || hex == m_chipColorUser) return;
    m_chipColorUser = hex;
    store(kChipColorUser, hex);
    emit changed();
}

void Settings::setChipColorId(const QString &hex)
{
    if (!isHexColor(hex) || hex == m_chipColorId) return;
    m_chipColorId = hex;
    store(kChipColorId, hex);
    emit changed();
}

void Settings::setChipColorDetail(const QString &hex)
{
    if (!isHexColor(hex) || hex == m_chipColorDetail) return;
    m_chipColorDetail = hex;
    store(kChipColorDetail, hex);
    emit changed();
}

void Settings::resetChipColors()
{
    m_chipColorPrimary = defaultChipColorPrimary();
    m_chipColorUser    = defaultChipColorUser();
    m_chipColorId      = defaultChipColorId();
    m_chipColorDetail  = defaultChipColorDetail();
    store(kChipColorPrimary, m_chipColorPrimary);
    store(kChipColorUser,    m_chipColorUser);
    store(kChipColorId,      m_chipColorId);
    store(kChipColorDetail,  m_chipColorDetail);
    emit changed();
}
