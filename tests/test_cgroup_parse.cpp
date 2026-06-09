// Tests for the cgroup parser/classifier. Pure header-only logic; covers
// real-world /proc/<pid>/cgroup samples from docker, containerd, k8s,
// podman, lxc, systemd and host scopes.

#include <QTest>

#include "backend/linux/CgroupParse.h"

using qiftop::backend::cgroup::classifyPath;
using qiftop::backend::cgroup::classifyPathChain;
using qiftop::backend::cgroup::classifyProcCgroup;
using qiftop::backend::cgroup::extractPath;
using qiftop::backend::cgroup::CgroupHint;

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

    void dockerLegacyFallbackDoesNotDuplicateSystemdScope()
    {
        const QString scoped64(64, QLatin1Char('a'));
        const QString legacy64(64, QLatin1Char('b'));
        const auto chain = qiftop::backend::cgroup::classifyPathChain(
            QStringLiteral("/system.slice/docker-%1.scope/docker/%2")
                .arg(scoped64, legacy64));
        QCOMPARE(chain.size(), 1);
        QCOMPARE(chain[0].runtime, QStringLiteral("docker"));
        QCOMPARE(chain[0].id, QString(12, QLatin1Char('a')));
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

    // RUNTIMES-M3: Podman with `--cgroup-manager cgroupfs` (older
    // cgroup-v1 hosts, or explicit config). Path segment is
    // `libpod-<64hex>` WITHOUT `.scope`. Pre-fix this collided with the
    // unanchored rxKubepodsPod and showed up as
    // {kubernetes, "-<63hex>"}, both wrong.
    void podmanLibpodCgroupfs()
    {
        const auto info = classifyPath(QStringLiteral(
            "/machine.slice/libpod-cafebabe00112233445566778899001122334455667788990011223344556677"));
        QVERIFY2(info.has_value(),
                 "libpod-<hex> without .scope must classify as podman");
        QCOMPARE(info->runtime, QStringLiteral("podman"));
        QCOMPARE(info->id,      QStringLiteral("cafebabe0011"));
    }

    // Also pin the BARE form (no parent slice — possible on rootless
    // setups under /user.slice/... before the /user.slice exclusion
    // guard kicks in, or on minimal cgroup hierarchies).
    void podmanLibpodCgroupfsBare()
    {
        const auto info = classifyPath(QStringLiteral(
            "/libpod-cafebabe00112233445566778899001122334455667788990011223344556677"));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("podman"));
        QCOMPARE(info->id,      QStringLiteral("cafebabe0011"));
    }

    // RUNTIMES-M1: CRI-O cgroupfs cgroup driver produces an identical
    // kubepods leaf path to containerd cgroupfs. By default we label
    // it containerd (matches Tracee + ecosystem convention). With
    // CgroupHint::PreferCrio supplied — what CgroupClassifier does
    // when /run/crio/crio.sock exists — the same leaf is labelled
    // cri-o instead, and `runtime=cri-o` filters work.
    void crioCgroupfsDefaultsToContainerd()
    {
        const QString path =
            QStringLiteral("/kubepods/besteffort/pod665b0949-7b83-49a8-a8df-1c50ac9b9d8c/"
                           "deadbeef00112233445566778899001122334455667788990011223344556677");
        const auto chain = classifyPathChain(path);  // Auto hint
        QCOMPARE(chain.size(), 2);
        QCOMPARE(chain.first().runtime, QStringLiteral("kubernetes"));
        QCOMPARE(chain.last().runtime,  QStringLiteral("containerd"));
        QCOMPARE(chain.last().id,       QStringLiteral("deadbeef0011"));
    }

    void crioCgroupfsWithHintLabelsAsCrio()
    {
        const QString path =
            QStringLiteral("/kubepods/besteffort/pod665b0949-7b83-49a8-a8df-1c50ac9b9d8c/"
                           "deadbeef00112233445566778899001122334455667788990011223344556677");
        const auto chain = classifyPathChain(path, CgroupHint::PreferCrio);
        QCOMPARE(chain.size(), 2);
        QCOMPARE(chain.first().runtime, QStringLiteral("kubernetes"));
        QCOMPARE(chain.last().runtime,  QStringLiteral("cri-o"));
        QCOMPARE(chain.last().id,       QStringLiteral("deadbeef0011"));
    }

    // Hint must NOT affect the systemd-driver CRI-O path (which carries
    // a canonical crio-<id>.scope segment and is unambiguous).
    void crioSystemdSchemeUnaffectedByHint()
    {
        const QString path =
            QStringLiteral("/kubepods.slice/kubepods-besteffort.slice/"
                           "kubepods-besteffort-pod665b0949_7b83_49a8_a8df_1c50ac9b9d8c.slice/"
                           "crio-deadbeef00112233445566778899001122334455667788990011223344556677.scope");
        const auto chain  = classifyPathChain(path);
        const auto chain2 = classifyPathChain(path, CgroupHint::PreferCrio);
        QCOMPARE(chain.size(), 2);
        QCOMPARE(chain.last().runtime, QStringLiteral("cri-o"));
        QCOMPARE(chain2.size(), 2);
        QCOMPARE(chain2.last().runtime, QStringLiteral("cri-o"));
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

    // ---- nested-container leaf-wins discipline --------------------------

    void k3sPodInDockerPrefersInnermost()
    {
        // The k3d "k3s-in-docker" case that prompted this rule: the
        // outer cgroup is a plain docker container; nested inside is a
        // kubepods slice with a cri-containerd-<id>.scope leaf for the
        // pod's container. We MUST report containerd / the inner CID,
        // not docker / the outer one — otherwise the UI shows the k3d
        // node as the owner of every pod's traffic.
        const QString outer64(64, QLatin1Char('a'));  // outer docker id
        const QString inner64(64, QLatin1Char('b'));  // inner containerd id
        const QString path = QStringLiteral(
            "/system.slice/docker-%1.scope"
            "/kubepods.slice/kubepods-besteffort.slice"
            "/kubepods-besteffort-pod665b0949_7b83_11ea_bc55_42010a8002b0.slice"
            "/cri-containerd-%2.scope").arg(outer64, inner64);
        const auto info = classifyPath(path);
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("containerd"));
        QCOMPARE(info->id, QString(12, QLatin1Char('b')));

        // Full chain: outer docker → kubepods pod → inner containerd.
        const auto chain = qiftop::backend::cgroup::classifyPathChain(path);
        QCOMPARE(chain.size(), 3);
        QCOMPARE(chain[0].runtime, QStringLiteral("docker"));
        QCOMPARE(chain[0].id, QString(12, QLatin1Char('a')));
        QCOMPARE(chain[1].runtime, QStringLiteral("kubernetes"));
        QVERIFY(chain[1].id.startsWith(QStringLiteral("665b0949")));
        QCOMPARE(chain[2].runtime, QStringLiteral("containerd"));
        QCOMPARE(chain[2].id, QString(12, QLatin1Char('b')));
    }

    void dockerInDockerPrefersInnermost()
    {
        // Plain dind with systemd driver inside the inner daemon too:
        // both segments are docker-<id>.scope. Inner wins.
        const QString outer64(64, QLatin1Char('a'));
        const QString inner64(64, QLatin1Char('b'));
        const auto info = classifyPath(QStringLiteral(
            "/system.slice/docker-%1.scope/docker-%2.scope")
                .arg(outer64, inner64));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("docker"));
        QCOMPARE(info->id, QString(12, QLatin1Char('b')));
    }

    void k8sCrioPodInDockerPrefersInnermost()
    {
        const QString outer64(64, QLatin1Char('1'));
        const QString inner64(64, QLatin1Char('2'));
        const auto info = classifyPath(QStringLiteral(
            "/system.slice/docker-%1.scope"
            "/kubepods.slice/kubepods-pod00000000_0000_0000_0000_000000000000.slice"
            "/crio-%2.scope").arg(outer64, inner64));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("cri-o"));
        QCOMPARE(info->id, QString(12, QLatin1Char('2')));
    }

    void k8sCgroupfsDriverK3dShape()
    {
        // The actual cgroup path observed inside a k3d v5.7 cluster
        // (verified live in the test VM): cgroupfs driver, so the
        // inner kubepods tree has no .slice/.scope suffixes and the
        // containerd leaf is a naked 64-hex segment.
        const auto path = QStringLiteral(
            "/system.slice/docker-f7690a04877e9ce57ee23958cd1a7bac"
            "94f8606fbebed43f84ef478f851970f0.scope"
            "/kubepods/besteffort/podf53c3981-8232-4261-ba57-7b0f5f4ea3ec"
            "/847e0a2e4c8b4cf757f5ad34b870ad65d61b10fcb02da448dba0f831b8b23aeb");
        const auto info = classifyPath(path);
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("containerd"));
        QCOMPARE(info->id, QStringLiteral("847e0a2e4c8b"));

        // Chain: docker (k3d node) → kubernetes (pod) → containerd (leaf).
        const auto chain = qiftop::backend::cgroup::classifyPathChain(path);
        QCOMPARE(chain.size(), 3);
        QCOMPARE(chain[0].runtime, QStringLiteral("docker"));
        QCOMPARE(chain[0].id, QStringLiteral("f7690a04877e"));
        QCOMPARE(chain[1].runtime, QStringLiteral("kubernetes"));
        QCOMPARE(chain[1].id, QStringLiteral("f53c3981-823"));
        QCOMPARE(chain[2].runtime, QStringLiteral("containerd"));
        QCOMPARE(chain[2].id, QStringLiteral("847e0a2e4c8b"));
    }

    void k8sNakedCgroupfsDriver()
    {
        // Naked k8s (k0s/kubeadm/kubelet directly on host) with the
        // cgroupfs driver: same kubepods/pod/<64hex> shape as k3d's
        // INNER segments but WITHOUT the docker wrapper. Chain must
        // be depth 2 (kubernetes → containerd), never 3.
        const QString cid64(64, QLatin1Char('c'));
        const auto path = QStringLiteral(
            "/kubepods/besteffort/poddeadbeef-1234-5678-9abc-def012345678"
            "/%1").arg(cid64);
        const auto chain = qiftop::backend::cgroup::classifyPathChain(path);
        QCOMPARE(chain.size(), 2);
        QCOMPARE(chain[0].runtime, QStringLiteral("kubernetes"));
        QCOMPARE(chain[0].id, QStringLiteral("deadbeef-123"));
        QCOMPARE(chain[1].runtime, QStringLiteral("containerd"));
        QCOMPARE(chain[1].id, QString(12, QLatin1Char('c')));
    }

    void k8sNakedSystemdDriver()
    {
        // Naked k8s with the systemd cgroup driver (kubelet
        // --cgroup-driver=systemd): kubepods.slice + cri-containerd
        // scope leaf, no docker wrapper. Chain depth 2.
        const QString cid64(64, QLatin1Char('c'));
        const auto path = QStringLiteral(
            "/kubepods.slice/kubepods-besteffort.slice"
            "/kubepods-besteffort-poddeadbeef_1234_5678_9abc_def012345678.slice"
            "/cri-containerd-%1.scope").arg(cid64);
        const auto chain = qiftop::backend::cgroup::classifyPathChain(path);
        QCOMPARE(chain.size(), 2);
        QCOMPARE(chain[0].runtime, QStringLiteral("kubernetes"));
        QVERIFY(chain[0].id.startsWith(QStringLiteral("deadbeef")));
        QCOMPARE(chain[1].runtime, QStringLiteral("containerd"));
        QCOMPARE(chain[1].id, QString(12, QLatin1Char('c')));
    }

    void kubepodsFallbackWhenNoRuntimeScope()
    {
        // If the leaf isn't a recognised runtime scope but the path
        // contains a pod slice, label as kubernetes with the pod UID.
        const auto info = classifyPath(QStringLiteral(
            "/kubepods.slice/kubepods-burstable.slice"
            "/kubepods-burstable-pod665b0949_7b83_11ea_bc55_42010a8002b0.slice"));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("kubernetes"));
        QVERIFY(info->id.startsWith(QStringLiteral("665b0949")));
    }

    void nspawnMachinectl()
    {
        // Containers booted by `machinectl start NAME` or
        // `systemd-nspawn --machine=NAME --boot ...`. The process
        // attribution-target is INSIDE the container, so we walk
        // through several segments of the guest's own systemd
        // hierarchy under .../payload/.
        using qiftop::backend::cgroup::classifyPathChain;
        const auto path = QStringLiteral(
            "/machine.slice/machine-alpine.scope"
            "/payload/system.slice/nginx.service");
        const auto chain = classifyPathChain(path);
        QCOMPARE(chain.size(), 1);
        QCOMPARE(chain[0].runtime, QStringLiteral("nspawn"));
        QCOMPARE(chain[0].id, QStringLiteral("alpine"));

        // Leaf-wins single-answer view returns the same.
        const auto info = classifyPath(path);
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("nspawn"));
        QCOMPARE(info->id, QStringLiteral("alpine"));
    }

    void nspawnTemplatedService()
    {
        // `systemctl start systemd-nspawn@NAME` form. Without the
        // explicit handler the systemd-unit fallback would label this
        // `systemd:unit:systemd-nspawn@NAME.service` — strictly true
        // but unhelpful (the runtime IS nspawn).
        using qiftop::backend::cgroup::classifyPathChain;
        const auto path = QStringLiteral(
            "/system.slice/system-systemd\\x2dnspawn.slice"
            "/systemd-nspawn@debian.service");
        const auto chain = classifyPathChain(path);
        QCOMPARE(chain.size(), 1);
        QCOMPARE(chain[0].runtime, QStringLiteral("nspawn"));
        QCOMPARE(chain[0].id, QStringLiteral("debian"));
    }

    void nspawnNameWithSystemdEscapes()
    {
        // Machine names containing characters systemd hex-escapes (dots,
        // dashes in some positions). The captured id is verbatim — same
        // form `machinectl list` and journal logs show.
        using qiftop::backend::cgroup::classifyPath;
        const auto info = classifyPath(QStringLiteral(
            "/machine.slice/machine-my\\x2dvm\\x2eorg.scope/payload/init.scope"));
        QVERIFY(info.has_value());
        QCOMPARE(info->runtime, QStringLiteral("nspawn"));
        QCOMPARE(info->id, QStringLiteral("my\\x2dvm\\x2eorg"));
    }

    // ---- classifyPathChain depth bound ----------------------------------

    void chainCapsAtMaxDepth()
    {
        using qiftop::backend::cgroup::classifyPathChain;
        using qiftop::backend::cgroup::kMaxContainerChainDepth;

        // Build a synthetic path with way more nested docker scopes than
        // the cap allows. The chain must come back length == cap and
        // contain the INNERMOST entries. Each id is keyed by `i` in
        // the LEADING 12 hex chars (left-aligned) so `.left(12)` short
        // ids differ between segments.
        QString path;
        constexpr int kSegments = kMaxContainerChainDepth + 5;
        for (int i = 0; i < kSegments; ++i) {
            const QString id =
                QStringLiteral("%1").arg(i, 12, 16, QLatin1Char('0'))
                + QString(52, QLatin1Char('0'));   // pad rhs to 64 chars
            path += QStringLiteral("/docker-%1.scope").arg(id);
        }
        // A qWarning is expected; allow it through unhindered.
        const auto chain = classifyPathChain(path);
        QCOMPARE(chain.size(), kMaxContainerChainDepth);
        // First kept entry corresponds to segment index `5` (outermost
        // 5 dropped: kSegments - kMax == 5).
        QCOMPARE(chain.first().id,
                 QStringLiteral("%1").arg(5, 12, 16, QLatin1Char('0')));
        // Last entry must be the leaf-most scope.
        QCOMPARE(chain.last().id,
                 QStringLiteral("%1").arg(kSegments - 1, 12, 16, QLatin1Char('0')));
    }

    void chainEmptyForHostScopes()
    {
        using qiftop::backend::cgroup::classifyPathChain;
        QVERIFY(classifyPathChain(QStringLiteral("/")).isEmpty());
        QVERIFY(classifyPathChain(QStringLiteral("/init.scope")).isEmpty());
        QVERIFY(classifyPathChain(QString()).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCgroupParse)
#include "test_cgroup_parse.moc"
