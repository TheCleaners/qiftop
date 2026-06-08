// Unit tests for qiftop::agent::loadIdleConfig — the INI loader that
// reads /etc/qiftop/agent.conf. Pre-extraction this lived as a file-
// static helper in agent/main.cpp and was effectively untestable; the
// Application/Config extraction was made specifically to unblock these.

#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include "agent/Config.h"
#include "agent/IdleManager.h"

namespace {

// Writes `body` to `<tmp>/agent.conf` and returns the absolute path.
QString writeConf(const QTemporaryDir &dir, const QByteArray &body)
{
    const QString path = dir.filePath(QStringLiteral("agent.conf"));
    QFile f(path);
    // NOTE: Q_ASSERT is a no-op in release builds — don't put the open()
    // call inside it. Use a real check so this helper works under both
    // Debug and Release CMAKE_BUILD_TYPE.
    const bool opened = f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    if (!opened) qWarning() << "writeConf: open failed:" << f.errorString();
    f.write(body);
    f.close();
    return path;
}

} // namespace

class TestAgentConfig : public QObject {
    Q_OBJECT

private slots:
    void missingFileReturnsDefaults()
    {
        // No file present at the path — must NOT throw, must NOT touch
        // disk, must return the default-constructed Config (so /etc-less
        // dev runs keep working).
        const auto cfg = qiftop::agent::loadIdleConfig(
            QStringLiteral("/nonexistent/path/agent.conf"));
        const qiftop::agent::IdleManager::Config defaults;
        QCOMPARE(cfg.activeIntervalMs, defaults.activeIntervalMs);
        QCOMPARE(cfg.idleTimeoutMs,    defaults.idleTimeoutMs);
        QCOMPARE(cfg.minIntervalMs,    defaults.minIntervalMs);
        QCOMPARE(cfg.hintTtlMs,        defaults.hintTtlMs);
    }

    void parsesPollAndIdleKeys()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = writeConf(dir, R"([poll]
min_interval_ms=250
base_interval_ms=2000

[idle]
timeout_secs=120
hint_ttl_secs=15
)");
        const auto cfg = qiftop::agent::loadIdleConfig(path);
        QCOMPARE(cfg.minIntervalMs,    250);
        QCOMPARE(cfg.activeIntervalMs, 2000);
        QCOMPARE(cfg.idleTimeoutMs,    120 * 1000);
        QCOMPARE(cfg.hintTtlMs,        15 * 1000);
    }

    void parsesScheduleString()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = writeConf(dir, R"([idle]
schedule=10:1500,20:3000,30:0
)");
        const auto cfg = qiftop::agent::loadIdleConfig(path);
        QCOMPARE(cfg.activeWindowMs,  10 * 1000);
        QCOMPARE(cfg.slow1IntervalMs, 1500);
        QCOMPARE(cfg.slow1WindowMs,   20 * 1000);
        QCOMPARE(cfg.slow2IntervalMs, 3000);
        QCOMPARE(cfg.slow2WindowMs,   30 * 1000);
    }

    void outOfRangeValuesFallBackToDefaults()
    {
        // 5 ms is below the 10 ms floor; 999 hours is above the 1-hour
        // ceiling. Both must be silently clamped to defaults (with a
        // qWarning each — we ignore that for the assertion's sake).
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = writeConf(dir, R"([poll]
min_interval_ms=5
base_interval_ms=3600001
)");
        const qiftop::agent::IdleManager::Config defaults;
        const auto cfg = qiftop::agent::loadIdleConfig(path);
        QCOMPARE(cfg.minIntervalMs,    defaults.minIntervalMs);
        QCOMPARE(cfg.activeIntervalMs, defaults.activeIntervalMs);
    }

    void unknownKeysAreTolerated()
    {
        // Forward-compat: an older agent reading a newer config file
        // must not crash on unknown keys.
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = writeConf(dir, R"([poll]
min_interval_ms=200
some_future_key=42

[future_section]
whatever=true
)");
        const auto cfg = qiftop::agent::loadIdleConfig(path);
        QCOMPARE(cfg.minIntervalMs, 200);
    }

    void malformedScheduleDoesNotClobberDefaults()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = writeConf(dir, R"([idle]
schedule=not-a-schedule
)");
        const qiftop::agent::IdleManager::Config defaults;
        const auto cfg = qiftop::agent::loadIdleConfig(path);
        // Pairs that don't match "<int>:<int>" are silently ignored;
        // every field keeps its compile-time default.
        QCOMPARE(cfg.activeWindowMs,  defaults.activeWindowMs);
        QCOMPARE(cfg.slow1IntervalMs, defaults.slow1IntervalMs);
        QCOMPARE(cfg.slow1WindowMs,   defaults.slow1WindowMs);
    }
};

QTEST_MAIN(TestAgentConfig)
#include "test_agent_config.moc"
