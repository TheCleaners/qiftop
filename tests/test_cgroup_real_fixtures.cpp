// Real-world /proc/<pid>/cgroup fixtures harvested from authoritative
// upstream sources (Docker docs, containerd/CRI docs, Kubernetes node
// admin docs, containers/podman tutorials, CRI-O cgroup doc, LXD
// systemd-cgroup doc). Each fixture is the EXACT byte content that a
// real /proc/<pid>/cgroup would contain on a host running that
// runtime/version combination. If a regex change breaks any of these,
// real users with that runtime will silently lose attribution — so
// these are pinned tests, not aspirational ones.
//
// Sources consulted (current as of 2026-06):
//   - docs.docker.com (cgroup drivers, runmetrics)
//   - kubernetes.io/docs/tasks/administer-cluster/kubelet-cgroup-driver/
//   - github.com/cri-o/cri-o/blob/main/docs/cgroup.md
//   - github.com/containers/podman/blob/main/docs/tutorials/rootless_cgroup_v2.md
//   - github.com/containerd/containerd/blob/main/docs/ops.md
//   - kernel.org cgroup-v2.html
//
// When adding a fixture: write the file under tests/fixtures/cgroup_real/
// and add ONE row to the data table in initTestCase_data(). The empty
// runtime+id strings encode "host scope — expect nullopt".

#include <QtTest>
#include <QFile>
#include <QString>

#include "backend/linux/CgroupParse.h"

using qiftop::backend::cgroup::classifyProcCgroup;

class TestCgroupRealFixtures : public QObject
{
    Q_OBJECT
private slots:
    void classify_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("expectedRuntime");
        QTest::addColumn<QString>("expectedIdPrefix");  // 12-char prefix

        //              fixture file name                  runtime          id prefix (or empty = nullopt)
        QTest::newRow("docker_v2_cgroupfs")        << "docker_v2_cgroupfs.txt"        << "docker"     << "3a4e1f7c9b8d";
        QTest::newRow("docker_v2_systemd")         << "docker_v2_systemd.txt"         << "docker"     << "3a4e1f7c9b8d";
        QTest::newRow("docker_v1_cgroupfs")        << "docker_v1_cgroupfs.txt"        << "docker"     << "3a4e1f7c9b8d";
        QTest::newRow("k8s_containerd_burstable")  << "k8s_containerd_burstable.txt"  << "containerd" << "7698bf823d17";
        QTest::newRow("k8s_containerd_guaranteed") << "k8s_containerd_guaranteed.txt" << "containerd" << "7698bf823d17";
        QTest::newRow("k8s_crio_besteffort")       << "k8s_crio_besteffort.txt"       << "cri-o"      << "f2b6e0560f1e";
        QTest::newRow("podman_rootless_v2")        << "podman_rootless_v2.txt"        << "podman"     << "abcdef012345";
        QTest::newRow("podman_rootful_v2")         << "podman_rootful_v2.txt"         << "podman"     << "abcdef012345";
        QTest::newRow("lxd_systemd")               << "lxd_systemd.txt"               << "lxd"        << "testcontaine";
        QTest::newRow("lxc_payload")               << "lxc_payload.txt"               << "lxc"        << "myguest";
        // systemd-nspawn: machinectl-registered (the modern default), both
        // when the resolved process is the container's init AND when it's
        // a service inside the booted container. The id is the human
        // machine name (NOT a content-addressable hash) — distinguishes
        // nspawn from every other supported runtime.
        QTest::newRow("nspawn_machinectl")         << "nspawn_machinectl.txt"         << "nspawn"     << "alpine";
        QTest::newRow("nspawn_machinectl_inside")  << "nspawn_machinectl_inside.txt"  << "nspawn"     << "fedora";
        QTest::newRow("nspawn_template_service")   << "nspawn_template_service.txt"   << "nspawn"     << "debian";
        QTest::newRow("host_init_scope")           << "host_init_scope.txt"           << ""           << "";
        QTest::newRow("host_user_session")         << "host_user_session.txt"         << ""           << "";
        QTest::newRow("host_systemd_service")      << "host_systemd_service.txt"      << "systemd"    << "unit:nginx.s";
        // RUNTIMES-H2 audit regression: user@<uid>.service is the
        // per-user systemd manager scope; misclassifying it as a
        // systemd-unit container would relabel every Wayland helper,
        // browser tab, and desktop service on every Linux desktop.
        // The /user.slice guard in classifyPathChain ensures this
        // (and any deeper user-managed app) stays "(host)".
        QTest::newRow("host_user_systemd_manager") << "host_user_systemd_manager.txt" << ""           << "";
        QTest::newRow("host_user_app_service")     << "host_user_app_service.txt"     << ""           << "";
    }

    void classify()
    {
        QFETCH(QString, file);
        QFETCH(QString, expectedRuntime);
        QFETCH(QString, expectedIdPrefix);

        const QString path = QStringLiteral(QIFTOP_FIXTURE_DIR) + u'/' + file;
        QFile f(path);
        QVERIFY2(f.open(QIODevice::ReadOnly),
                 qPrintable(QStringLiteral("cannot open %1").arg(path)));
        const QString contents = QString::fromUtf8(f.readAll());

        const auto info = classifyProcCgroup(contents);

        if (expectedRuntime.isEmpty()) {
            QVERIFY2(!info.has_value(),
                     qPrintable(QStringLiteral("expected host scope but got runtime=%1 id=%2")
                                    .arg(info ? info->runtime : QString())
                                    .arg(info ? info->id      : QString())));
            return;
        }

        QVERIFY2(info.has_value(),
                 qPrintable(QStringLiteral("expected runtime=%1 id~%2 but got nullopt")
                                .arg(expectedRuntime, expectedIdPrefix)));
        QCOMPARE(info->runtime, expectedRuntime);
        QVERIFY2(info->id.startsWith(expectedIdPrefix),
                 qPrintable(QStringLiteral("expected id prefix '%1' but got '%2'")
                                .arg(expectedIdPrefix, info->id)));
    }
};

QTEST_GUILESS_MAIN(TestCgroupRealFixtures)
#include "test_cgroup_real_fixtures.moc"
