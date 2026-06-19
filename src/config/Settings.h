#pragma once

#include <QObject>
#include <QSettings>

// Application-wide preferences, persisted via QSettings.
//
// Each setter writes through to QSettings and emits changed() so any UI piece
// holding a reference can react. Construct exactly one instance in main() and
// pass it to consumers (no global singleton).
class Settings : public QObject {
    Q_OBJECT

public:
    explicit Settings(QObject *parent = nullptr);

    [[nodiscard]] int  pollIntervalMs() const   { return m_pollIntervalMs; }
    [[nodiscard]] bool showLoopback() const     { return m_showLoopback; }
    [[nodiscard]] bool showDown() const         { return m_showDown; }
    [[nodiscard]] bool ipv6Enabled() const      { return m_ipv6Enabled; }
    [[nodiscard]] bool resolveHostnames() const { return m_resolveHostnames; }
    [[nodiscard]] bool closeToTray() const      { return m_closeToTray; }
    [[nodiscard]] QStringList trayInterfaces() const { return m_trayInterfaces; }
    [[nodiscard]] QStringList connectionVisibleIfaces() const { return m_connVisibleIfaces; }
    // How long a closed connection lingers in the table as "stale" (italic,
    // greyed) before being pruned. TCP/ICMP and "other" use the first value;
    // UDP gets its own (typically higher, since UDP flows are intrinsically
    // bursty and kernel conntrack times them out aggressively).
    [[nodiscard]] int connectionStaleRetentionSecs() const    { return m_connStaleRetentionSecs; }
    [[nodiscard]] int connectionStaleRetentionSecsUdp() const { return m_connStaleRetentionSecsUdp; }
    // When true, UDP rows sharing a peer (remote address+port for outbound
    // flows, local address+port for inbound flows) are coalesced into a
    // single row whose ephemeral side displays as "*". Saves a lot of
    // visual noise from short-lived DNS / one-shot CLI tools.
    [[nodiscard]] bool udpAggregateByPeer() const            { return m_udpAggregateByPeer; }
    // When true (and DNS resolution is on), this host's own interface
    // addresses (e.g. 192.168.1.42) are rendered as "localhost" — matching
    // how a user thinks about the local end of a flow. Loopback addresses
    // (127.0.0.1, ::1) are always shown as "localhost" regardless.
    [[nodiscard]] bool resolveIfaceAddrsAsLocalhost() const  { return m_resolveIfaceAddrsAsLocalhost; }
    // When true, the Connections "Flow" column color-codes its components:
    // a tinted proto tag, a "cool" color for the source endpoint, a "warm"
    // color for the destination. When false, everything uses the palette's
    // Text color (a single visual style for the whole line).
    [[nodiscard]] bool colorCodeConnectionFlow() const       { return m_colorCodeConnectionFlow; }
    // When true (and color-coding is on), the entire connection row is
    // tinted by direction: faint green for outbound, faint red for
    // inbound. No tint for Unknown-direction flows.
    [[nodiscard]] bool tintRowByDirection() const            { return m_tintRowByDirection; }
    // Protocol-family visibility for the Connections tab. Independent
    // toggles so users can hide noisy UDP or focus on TCP only.
    [[nodiscard]] bool showTcp() const                       { return m_showTcp; }
    [[nodiscard]] bool showUdp() const                       { return m_showUdp; }
    // Per-connection adaptive throughput gauge: when on, each row's
    // background tint splits into a "filled" (darker) portion proportional
    // to the current combined rx+tx rate vs. an adaptive reference, plus a
    // "Max RX"/"Max TX" pair of columns showing that reference value.
    [[nodiscard]] bool throughputGaugeEnabled() const        { return m_throughputGaugeEnabled; }
    // Row-spanning bandwidth bar on the Interfaces tab (parity with the
    // nqiftop interface gauge). Independent of the connections gauge above and
    // ON by default — it's the headline of that tab and low-density.
    [[nodiscard]] bool interfaceGaugeEnabled() const         { return m_interfaceGaugeEnabled; }
    // How the gauge's reference value is computed:
    //   Windowed         = max of recent rates over a sliding time window
    //   CumulativeAverage = lifetime CMA of the connection's rate samples
    enum class ThroughputMaxMode : int { Windowed = 0, CumulativeAverage = 1 };
    [[nodiscard]] ThroughputMaxMode throughputMaxMode() const { return m_throughputMaxMode; }

    // Connections view layout. Flat = v0.1 iftop-style table. The grouped
    // modes turn the view into a tree where each top-level row aggregates
    // the SUM of its children's rates and is annotated with a flow count.
    // Children are individual flows, rendered identically to Flat mode
    // so the color-coded gauge / direction tint stay meaningful.
    enum class ConnectionViewMode : int {
        Flat = 0,
        ByInterface = 1,
        ByContainer = 2,
        ByProcess = 3,
    };
    [[nodiscard]] ConnectionViewMode connectionViewMode() const { return m_connViewMode; }
    // Process / Container attribution column visibility. Gated in the
    // Settings dialog by the agent's process-attribution-wire /
    // container-attribution-wire capability tokens, but the values
    // persist regardless so they survive switching backends. The
    // "show chain in tooltip" toggle is independent — it controls
    // whether the Container column's tooltip lists the full
    // OUTER→INNER nesting (container-chain-wire token).
    [[nodiscard]] bool showProcessColumn() const   { return m_showProcessColumn; }
    [[nodiscard]] bool showContainerColumn() const { return m_showContainerColumn; }
    [[nodiscard]] bool showContainerChainInTooltip() const { return m_showContainerChainInTooltip; }
    [[nodiscard]] bool showGroupHeaderDetails() const { return m_showGroupHeaderDetails; }
    [[nodiscard]] bool sortWithinGroups() const { return m_sortWithinGroups; }

    // --- Group-header chip palette (configurable; distinct from the
    //     peer src/dst colours used in the flow column). Four semantic
    //     roles map the chip kinds: primary (process/container/iface
    //     name), user (uid→name), id (container id), detail (pid /
    //     cmdline / flow count). Stored + returned as #rrggbb strings
    //     (Settings stays Qt6::Core-only — the UI converts to QColor);
    //     defaults are deliberately off the blue/amber peer palette. ---
    [[nodiscard]] QString chipColorPrimary() const { return m_chipColorPrimary; }
    [[nodiscard]] QString chipColorUser()    const { return m_chipColorUser; }
    [[nodiscard]] QString chipColorId()      const { return m_chipColorId; }
    [[nodiscard]] QString chipColorDetail()  const { return m_chipColorDetail; }

    [[nodiscard]] static QString defaultChipColorPrimary() { return QStringLiteral("#b58bff"); } // violet
    [[nodiscard]] static QString defaultChipColorUser()    { return QStringLiteral("#5fd0c5"); } // teal
    [[nodiscard]] static QString defaultChipColorId()      { return QStringLiteral("#e08ab8"); } // rose
    [[nodiscard]] static QString defaultChipColorDetail()  { return QStringLiteral("#9aa0a6"); } // grey
    [[nodiscard]] int  throughputWindowSecs() const          { return m_throughputWindowSecs; }
    // EMA time constant (milliseconds) for smoothing per-connection
    // instantaneous rx/tx rates. 0 = no smoothing (raw per-tick deltas).
    // Larger values produce smoother but laggier rate readouts; the
    // throughput gauge and Max columns also see the smoothed values
    // since they're derived from the same rate. Sub-second values are
    // supported (e.g. 250 = a quarter-second time constant).
    [[nodiscard]] int  rateSmoothingMs() const               { return m_rateSmoothingMs; }
    // When true, the window title bar is suffixed with a compact summary
    // of the current filter/cadence state (poll interval, active iface
    // filter, protocol & family toggles). Useful when running multiple
    // qiftop windows side-by-side or with different filters.
    [[nodiscard]] bool showStatusInTitle() const             { return m_showStatusInTitle; }
    // Whether to install a per-user XDG autostart entry so qiftop is
    // launched at desktop login (silently, into the tray). Source of
    // truth is the filesystem under ~/.config/autostart/qiftop.desktop
    // — not a QSettings key — so external tools (gnome-tweaks,
    // systemsettings) stay authoritative and changes there are
    // reflected next time this getter is called.
    [[nodiscard]] bool startOnLogin() const;
    // Free-form filter expression applied to the Connections table.
    // Empty = no filter. See util/ConnectionFilter.h for grammar.
    [[nodiscard]] QString connectionFilterExpr() const { return m_connFilterExpr; }

    // Name of the active GUI colour theme (see src/ui/GuiTheme.h). Default
    // "System" means "don't override the native Qt palette/style" — so the
    // out-of-the-box look is unchanged. Persisted as a plain string so an
    // unknown name (e.g. from a future build) cleanly falls back to System.
    [[nodiscard]] QString guiThemeName() const { return m_guiThemeName; }

    void setPollIntervalMs(int ms);
    void setShowLoopback(bool v);
    void setShowDown(bool v);
    void setIpv6Enabled(bool v);
    void setResolveHostnames(bool v);
    void setCloseToTray(bool v);
    void setTrayInterfaces(const QStringList &v);
    // Empty list = no filter (show all). Non-empty = restrict to these names;
    // include the empty string "" to also keep flows whose iface couldn't be
    // attributed by the backend.
    void setConnectionVisibleIfaces(const QStringList &v);
    // Same as above, but does NOT persist to disk. Used for command-line
    // overrides (qiftop -i <iface>) that should affect only the current
    // session. Persistence resumes the next time the user changes it via
    // the UI (which goes through the persisting setter).
    void setConnectionVisibleIfacesTransient(const QStringList &v);
    void setConnectionStaleRetentionSecs(int secs);
    void setConnectionStaleRetentionSecsUdp(int secs);
    void setUdpAggregateByPeer(bool v);
    void setResolveIfaceAddrsAsLocalhost(bool v);
    void setColorCodeConnectionFlow(bool v);
    void setTintRowByDirection(bool v);
    void setShowTcp(bool v);
    void setShowUdp(bool v);
    void setThroughputGaugeEnabled(bool v);
    void setInterfaceGaugeEnabled(bool v);
    void setThroughputMaxMode(ThroughputMaxMode m);
    void setThroughputWindowSecs(int secs);
    void setRateSmoothingMs(int ms);
    void setShowStatusInTitle(bool v);
    void setStartOnLogin(bool v);
    void setConnectionFilterExpr(const QString &expr);
    void setGuiThemeName(const QString &name);
    void setConnectionViewMode(ConnectionViewMode m);
    void setShowProcessColumn(bool v);
    void setShowContainerColumn(bool v);
    void setShowContainerChainInTooltip(bool v);
    void setShowGroupHeaderDetails(bool v);
    void setSortWithinGroups(bool v);

    void setChipColorPrimary(const QString &hex);
    void setChipColorUser(const QString &hex);
    void setChipColorId(const QString &hex);
    void setChipColorDetail(const QString &hex);
    // Restore all four chip colours to their defaults (single changed()).
    void resetChipColors();

signals:
    void changed();

private:
    void load();
    void store(const char *key, const QVariant &value);

    QSettings m_store;

    // False when running privileged with a config dir owned by another user
    // (sudo -E / GUI self-elevation): settings are loaded but never written,
    // so we don't litter the invoking user's ~/.config with root-owned files.
    bool m_persist = true;

    int  m_pollIntervalMs   = 1000;
    bool m_showLoopback     = false;
    bool m_showDown         = true;
    bool m_ipv6Enabled      = true;
    bool m_resolveHostnames = false;
    bool m_closeToTray      = true;
    QStringList m_trayInterfaces;
    QStringList m_connVisibleIfaces;
    int  m_connStaleRetentionSecs        = 15;   // TCP/ICMP/other linger
    int  m_connStaleRetentionSecsUdp     = 60;   // UDP linger (bursty)
    bool m_udpAggregateByPeer            = true;
    bool m_resolveIfaceAddrsAsLocalhost  = true;
    bool m_colorCodeConnectionFlow       = true;
    bool m_tintRowByDirection            = false;
    bool m_showTcp                       = true;
    bool m_showUdp                       = true;
    bool m_throughputGaugeEnabled        = false;
    bool m_interfaceGaugeEnabled         = true;
    ThroughputMaxMode m_throughputMaxMode = ThroughputMaxMode::Windowed;
    int  m_throughputWindowSecs          = 30;
    int  m_rateSmoothingMs               = 0;   // 0 = off (EMA τ in ms; sub-second supported)
    bool m_showStatusInTitle             = false;
    QString m_connFilterExpr;
    QString m_guiThemeName               = QStringLiteral("System");
    ConnectionViewMode m_connViewMode    = ConnectionViewMode::Flat;
    // Default-shown: the attribution data is already on the wire (no extra
    // cost) and applySettingsToUi() AND-gates each column on the agent's
    // matching capability token, so the column stays hidden against an agent
    // (or in-process backend) that doesn't advertise attribution.
    bool m_showProcessColumn             = true;
    bool m_showContainerColumn           = true;
    bool m_showContainerChainInTooltip   = true;
    // When grouping (ByInterface/ByContainer/ByProcess), show extra
    // attribution detail inline on the group header rows (pid, user,
    // container id, etc). On by default — it's the point of grouping.
    bool m_showGroupHeaderDetails        = true;
    // When true (default), header clicks sort rows within each group and keep
    // the group order fixed; when false, the classic global group+row sort.
    bool m_sortWithinGroups              = true;
    QString m_chipColorPrimary = defaultChipColorPrimary();
    QString m_chipColorUser    = defaultChipColorUser();
    QString m_chipColorId      = defaultChipColorId();
    QString m_chipColorDetail  = defaultChipColorDetail();
};
