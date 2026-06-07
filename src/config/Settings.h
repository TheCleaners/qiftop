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
    // How the gauge's reference value is computed:
    //   Windowed         = max of recent rates over a sliding time window
    //   CumulativeAverage = lifetime CMA of the connection's rate samples
    enum class ThroughputMaxMode : int { Windowed = 0, CumulativeAverage = 1 };
    [[nodiscard]] ThroughputMaxMode throughputMaxMode() const { return m_throughputMaxMode; }
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
    void setThroughputMaxMode(ThroughputMaxMode m);
    void setThroughputWindowSecs(int secs);
    void setRateSmoothingMs(int ms);
    void setShowStatusInTitle(bool v);
    void setStartOnLogin(bool v);
    void setConnectionFilterExpr(const QString &expr);

signals:
    void changed();

private:
    void load();
    void store(const char *key, const QVariant &value);

    QSettings m_store;

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
    ThroughputMaxMode m_throughputMaxMode = ThroughputMaxMode::Windowed;
    int  m_throughputWindowSecs          = 30;
    int  m_rateSmoothingMs               = 0;   // 0 = off (EMA τ in ms; sub-second supported)
    bool m_showStatusInTitle             = false;
    QString m_connFilterExpr;
};
