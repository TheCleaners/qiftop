// Tests for the cgroup parser/classifier. Pure header-only logic; covers
// real-world /proc/<pid>/cgroup samples from docker, containerd, k8s,
// podman, lxc, systemd and host scopes.

#include <QTest>

#include "backend/linux/CgroupParse.h"

using qiftop::backend::cgroup::classifyPath;
using qiftop::backend::cgroup::classifyProcCgroup;
using qiftop::backend::cgroup::extractPath;

class TestCgroupParse : public QObject {
    Q_OBJECT
private slots:
    // ---- extractPath ----------------------------------------------------

    void v2UnifiedLine()
    {
        QCOMPARE(extractPath(u"0::/system.slice/docker.service\n"),
                 QStringLiteral("/system.slice/docker.service"));
    }

    void v1MultiLineFallback()
    {
        // Legacy cgroup v1: many lines, pick the first one with a path.
        const auto src = QStringLiteral(
            "12:pids:/docker/abcdef\n"
            "11:cpu,cpuacct:/docker/abcdef\n");
        QCOMPARE(extractPath(src), QStringLiteral("/docker/abcdef"));
    }

    void v2PrefersOverV1()
    {
        // Hybrid hosts list v1 controllers AND a v2 unified line. We want
        // the v2 path because it's the authoritative one.
        const auto src = QStringLiteral(
            "12:pids:/system.slice/foo.service\n"
            "0::/system.slice/foo.service\n");
        QCOMPARE(extractPath(src), QStringLiteral("/system.slice/foo.service"));
    }

    void emptyOrGarbage()
    {
        QVERIFY(extractPath(u"").isEmpty());
        QVERIFY(extractPath(u"garbage\n").isEmpty());
        QVERIFY(extractPath(u"0:single-colon\n").isEmpty());
    }

    // ---- classifyPath ---------------------------------------------------

    void hostScopesReturnNone()
    {
        QVERIFY(!classifyPath(QStringLiteral("/")).has_value());
        QVERIFY(!classifyPath(QStringLiteral("/init.scope")).has_value());
        QVERIFY(!classifyPath(QStringLiteral("")).has_value());
    }

    void dockerV1Path()
    {
        const auto info = classifyPath(QStringLiteral(
            "/docker/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("docker"));
        QCOMPARE(info->id,      QStringLiteral("0123456789ab"));
    }

    void dockerSystemdScope()
    {
        const auto info = classifyPath(QStringLiteral(
            "/system.slice/docker-abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789.scope"));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("docker"));
        QCOMPARE(info->id,      QStringLiteral("abcdef012345"));
    }

    void containerdCriScope()
    {
        const auto info = classifyPath(QStringLiteral(
            "/kubepods.slice/kubepods-burstable.slice/kubepods-burstable-pod1234.slice/"
            "cri-containerd-deadbeef1234567890abcdef1234567890abcdef1234567890abcdef12345678.scope"));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("containerd"));
        QCOMPARE(info->id,      QStringLiteral("deadbeef1234"));
    }

    void podmanLibpodScope()
    {
        const auto info = classifyPath(QStringLiteral(
            "/machine.slice/libpod-cafebabe00112233445566778899001122334455667788990011223344556677.scope"));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("podman"));
        QCOMPARE(info->id,      QStringLiteral("cafebabe0011"));
    }

    void lxcPayload()
    {
        const auto info = classifyPath(QStringLiteral("/lxc.payload.myguest/init.scope"));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("lxc"));
        QCOMPARE(info->id,      QStringLiteral("myguest"));
    }

    void lxcLegacy()
    {
        const auto info = classifyPath(QStringLiteral("/lxc/myguest"));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("lxc"));
        QCOMPARE(info->id,      QStringLiteral("myguest"));
    }

    void systemdUnitFromPath()
    {
        const auto info = classifyPath(QStringLiteral("/system.slice/nginx.service"));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("systemd"));
        QCOMPARE(info->id,      QStringLiteral("unit:nginx.service"));
    }

    void systemdSessionScopeIsNotContainer()
    {
        // Session scopes are just "user X is logged in" — not interesting.
        QVERIFY(!classifyPath(QStringLiteral(
                    "/user.slice/user-1000.slice/session-10.scope")).has_value());
    }

    // ---- classifyProcCgroup convenience ---------------------------------

    void classifyConvenienceUnifiedDocker()
    {
        const auto info = classifyProcCgroup(u"0::/system.slice/"
            "docker-abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789.scope\n");
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("docker"));
    }
};

QTEST_GUILESS_MAIN(TestCgroupParse)
#include "test_cgroup_parse.moc"
