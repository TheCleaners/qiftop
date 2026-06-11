#include "Config.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QtDebug>

#include "util/Logging.h"

namespace qiftop::agent {

namespace {

// Clamp a numeric config value into [lo, hi]; warn on adjustment so admins
// notice typos in /etc/qiftop/agent.conf instead of silently getting
// degenerate cadences (or, worse, an unbounded hinted cadence that bypasses
// the documented absolute floor).
template <typename T>
T clampCfg(const char *key, T raw, T lo, T hi, T fallback)
{
    if (raw < lo || raw > hi) {
        qWarning().noquote()
            << "agent: config key" << key << "value" << raw
            << "out of range [" << lo << "," << hi << "] — using" << fallback;
        return fallback;
    }
    return raw;
}

} // namespace

IdleManager::Config loadIdleConfig(const QString &path)
{
    IdleManager::Config cfg; // defaults
    if (!QFileInfo::exists(path)) {
        qCInfo(lcVerbose).noquote() << "agent: no config file at" << path
                                    << "— using built-in defaults";
        return cfg;
    }
    QSettings ini(path, QSettings::IniFormat);

    // Reasonable bounds: nothing below 10 ms (would be a hard DoS on the
    // netlink subsystem); nothing above one hour (clearly a typo).
    constexpr int kMinMs = 10;
    constexpr int kMaxMs = 60 * 60 * 1000;
    // Timeouts/windows can be zero (meaning "disable that step") or up to
    // ~24 hours; negative is always wrong.
    constexpr int kMaxWin = 24 * 60 * 60 * 1000;

    // Helper: read an INI key as seconds, clamp into [0, 24h], then multiply
    // into milliseconds in 64-bit and clamp into the int range. Doing the
    // multiplication in 32-bit on the raw INI value (which a typo could
    // easily make huge) is UB on overflow — the previous code clamped after
    // multiplying, which doesn't save you from the overflow itself.
    constexpr qint64 kMaxSec = 24 * 60 * 60;
    auto secsToMs = [&](const char *key, const QString &iniKey, int defaultMs) -> int {
        const qint64 raw = ini.value(iniKey, qint64(defaultMs) / 1000).toLongLong();
        if (raw < 0 || raw > kMaxSec) {
            qWarning().noquote()
                << "agent: config key" << key << "value" << raw
                << "seconds out of range [0," << kMaxSec << "] — using"
                << defaultMs / 1000;
            return defaultMs;
        }
        return int(raw * 1000);
    };
    auto secsToMsInline = [&](const char *key, qint64 raw, int defaultMs) -> int {
        if (raw < 0 || raw > kMaxSec) {
            qWarning().noquote()
                << "agent: config key" << key << "value" << raw
                << "seconds out of range [0," << kMaxSec << "] — using"
                << defaultMs / 1000;
            return defaultMs;
        }
        return int(raw * 1000);
    };

    cfg.minIntervalMs    = clampCfg("poll/min_interval_ms",
                                    ini.value(QStringLiteral("poll/min_interval_ms"),
                                              cfg.minIntervalMs).toInt(),
                                    kMinMs, kMaxMs, cfg.minIntervalMs);
    cfg.activeIntervalMs = clampCfg("poll/base_interval_ms",
                                    ini.value(QStringLiteral("poll/base_interval_ms"),
                                              cfg.activeIntervalMs).toInt(),
                                    cfg.minIntervalMs, kMaxMs, cfg.activeIntervalMs);
    cfg.idleTimeoutMs    = secsToMs("idle/timeout_secs",
                                    QStringLiteral("idle/timeout_secs"),
                                    cfg.idleTimeoutMs);
    cfg.hintTtlMs        = clampCfg("idle/hint_ttl_secs (ms)",
                                    secsToMs("idle/hint_ttl_secs",
                                             QStringLiteral("idle/hint_ttl_secs"),
                                             cfg.hintTtlMs),
                                    kMinMs, kMaxWin, cfg.hintTtlMs);

    // schedule = active_window_secs:slow1_ms,slow1_window_secs:slow2_ms,slow2_window_secs:0
    // We accept the simpler form: three "<window_secs>:<interval_ms>" pairs.
    const QString sched = ini.value(QStringLiteral("idle/schedule"),
                                    QStringLiteral("30:2000,45:5000,60:0")).toStringList().join(QLatin1Char(','));
    const QStringList pairs = sched.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (pairs.size() >= 1) {
        const auto parts = pairs[0].split(QLatin1Char(':'));
        if (parts.size() == 2) {
            cfg.activeWindowMs  = secsToMsInline("idle/schedule window1",
                                                 parts[0].trimmed().toLongLong(),
                                                 cfg.activeWindowMs);
            cfg.slow1IntervalMs = clampCfg("idle/schedule slow1 (ms)",
                                           parts[1].trimmed().toInt(),
                                           cfg.minIntervalMs, kMaxMs, cfg.slow1IntervalMs);
        }
    }
    if (pairs.size() >= 2) {
        const auto parts = pairs[1].split(QLatin1Char(':'));
        if (parts.size() == 2) {
            cfg.slow1WindowMs   = secsToMsInline("idle/schedule window2",
                                                 parts[0].trimmed().toLongLong(),
                                                 cfg.slow1WindowMs);
            cfg.slow2IntervalMs = clampCfg("idle/schedule slow2 (ms)",
                                           parts[1].trimmed().toInt(),
                                           cfg.minIntervalMs, kMaxMs, cfg.slow2IntervalMs);
        }
    }
    if (pairs.size() >= 3) {
        const auto parts = pairs[2].split(QLatin1Char(':'));
        if (parts.size() == 2) {
            cfg.slow2WindowMs = secsToMsInline("idle/schedule window3",
                                               parts[0].trimmed().toLongLong(),
                                               cfg.slow2WindowMs);
            // third interval is the "paused" sentinel; we keep idleTimeoutMs separate
        }
    }
    qCInfo(lcVerbose).noquote()
        << "agent: loaded config" << path
        << "active=" << cfg.activeIntervalMs << "ms"
        << "schedule:" << cfg.activeWindowMs/1000 << "s→" << cfg.slow1IntervalMs << "ms,"
        << cfg.slow1WindowMs/1000  << "s→" << cfg.slow2IntervalMs << "ms,"
        << "idle=" << cfg.idleTimeoutMs/1000 << "s";
    return cfg;
}

ProcessDetailsPolicy loadProcessDetailsPolicy(const QString &path)
{
    ProcessDetailsPolicy pol; // default: Owner
    if (!QFileInfo::exists(path))
        return pol;
    QSettings ini(path, QSettings::IniFormat);

    const QString mode = ini.value(QStringLiteral("process_details/disclosure"),
                                    QStringLiteral("owner")).toString().trimmed().toLower();
    if (mode == QLatin1String("owner"))
        pol.mode = ProcessDetailsPolicy::Mode::Owner;
    else if (mode == QLatin1String("permissive") || mode == QLatin1String("all"))
        pol.mode = ProcessDetailsPolicy::Mode::Permissive;
    else if (mode == QLatin1String("restricted") || mode == QLatin1String("users"))
        pol.mode = ProcessDetailsPolicy::Mode::Restricted;
    else {
        qWarning().noquote()
            << "agent: config key process_details/disclosure value" << mode
            << "unrecognised (owner|permissive|restricted) — using owner";
        pol.mode = ProcessDetailsPolicy::Mode::Owner;
    }

    const auto splitList = [&](const QString &key) {
        // Accept comma- or whitespace-separated lists; QSettings turns a
        // comma-containing INI value into a QStringList already.
        QStringList out;
        for (const QString &tok : ini.value(key).toStringList())
            for (const QString &p : tok.split(QRegularExpression(QStringLiteral("[\\s,]+")),
                                              Qt::SkipEmptyParts))
                out << p.trimmed();
        out.removeDuplicates();
        return out;
    };
    pol.allowUsers  = splitList(QStringLiteral("process_details/allow_users"));
    pol.allowGroups = splitList(QStringLiteral("process_details/allow_groups"));

    qCInfo(lcVerbose).noquote()
        << "agent: process-details disclosure =" << mode
        << "users=" << pol.allowUsers << "groups=" << pol.allowGroups;
    return pol;
}

} // namespace qiftop::agent
