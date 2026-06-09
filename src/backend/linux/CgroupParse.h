#pragma once

#include <QRegularExpression>
#include <QString>

#include <optional>

#include "backend/ProcessResolver.h"

// Pure-logic helpers for cgroup-based container classification. Split out
// of CgroupClassifier.cpp so the regex patterns can be unit-tested
// against representative real-world /proc/<pid>/cgroup paths without
// needing a running container runtime.

namespace qiftop::backend::cgroup {

// Extract the controller path from /proc/<pid>/cgroup file contents.
//
// The file has one line per cgroup hierarchy the process is in:
//   v2 (unified):  "0::/path"
//   v1 (legacy):   "<id>:<controller-list>:/path"
//
// Returns the v2 path if present (preferred — every modern Linux); else
// the path from the FIRST non-empty v1 line (any controller will do — they
// all encode the same container scope on systems that use cgroups for
// containerisation). Returns empty if no usable line found.
[[nodiscard]] inline QString extractPath(QStringView contents)
{
    QString v1Fallback;
    for (const auto &lineView : contents.split(u'\n', Qt::SkipEmptyParts)) {
        const QString line = lineView.toString();
        const int firstColon = line.indexOf(u':');
        if (firstColon < 0) continue;
        const int secondColon = line.indexOf(u':', firstColon + 1);
        if (secondColon < 0) continue;
        const QString controllers = line.mid(firstColon + 1, secondColon - firstColon - 1);
        const QString path = line.mid(secondColon + 1);
        if (controllers.isEmpty()) {
            // v2 unified line — done.
            return path;
        }
        if (v1Fallback.isEmpty()) v1Fallback = path;
    }
    return v1Fallback;
}

// Classify a cgroup path into a ContainerInfo. Empty/host-scope paths
// (typically `/`, `/init.scope`, plain `*.service` / `*.scope` under
// system.slice without container markers) return nullopt — these
// processes are the host itself, not a container.
//
// Patterns recognised (lowercase runtime token, 12-char short id):
//   docker            "/docker/<64hex>" or "docker-<64hex>.scope"
//   containerd / k8s  "cri-containerd-<64hex>.scope", "kubepods/.../<hex>"
//   podman            "libpod-<64hex>.scope"
//   lxc / lxd         "/lxc/<name>", "lxc.payload.<name>/"
//   systemd           "*.service" / "*.slice" outside of /user.slice and /system.slice
//                     (kept distinct from host so the UI can show e.g. "unit:nginx.service")
[[nodiscard]] inline std::optional<ContainerInfo> classifyPath(const QString &path)
{
    if (path.isEmpty() || path == QLatin1String("/")
        || path == QLatin1String("/init.scope")) {
        return std::nullopt;
    }

    // --- Docker -----------------------------------------------------------
    static const QRegularExpression rxDocker1(
        QStringLiteral("/docker[-/]([0-9a-f]{64})"));
    static const QRegularExpression rxDocker2(
        QStringLiteral("docker-([0-9a-f]{64})\\.scope"));
    if (const auto m = rxDocker1.match(path); m.hasMatch())
        return ContainerInfo{QStringLiteral("docker"), m.captured(1).left(12), {}};
    if (const auto m = rxDocker2.match(path); m.hasMatch())
        return ContainerInfo{QStringLiteral("docker"), m.captured(1).left(12), {}};

    // --- containerd / Kubernetes -----------------------------------------
    static const QRegularExpression rxCriContainerd(
        QStringLiteral("cri-containerd-([0-9a-f]{64})\\.scope"));
    if (const auto m = rxCriContainerd.match(path); m.hasMatch())
        return ContainerInfo{QStringLiteral("containerd"), m.captured(1).left(12), {}};

    // --- CRI-O ------------------------------------------------------------
    // Real path:  /kubepods.slice/.../crio-<64hex>.scope
    static const QRegularExpression rxCrio(
        QStringLiteral("(?:^|/)crio-([0-9a-f]{64})\\.scope"));
    if (const auto m = rxCrio.match(path); m.hasMatch())
        return ContainerInfo{QStringLiteral("cri-o"), m.captured(1).left(12), {}};

    // kubepods slice without a runtime-specific marker: still useful to label.
    // Real pod UIDs use underscores in the systemd slice name
    // (kubepods-burstable-pod665b0949_7b83_11ea_bc55_42010a8002b0.slice),
    // hence the underscore in the character class.
    static const QRegularExpression rxKubepods(
        QStringLiteral("kubepods[./].*?pod([0-9a-f_]{32,72})"));
    if (const auto m = rxKubepods.match(path); m.hasMatch())
        return ContainerInfo{QStringLiteral("kubernetes"), m.captured(1).left(12), {}};

    // --- Podman -----------------------------------------------------------
    static const QRegularExpression rxPodman(
        QStringLiteral("libpod-([0-9a-f]{64})\\.scope"));
    if (const auto m = rxPodman.match(path); m.hasMatch())
        return ContainerInfo{QStringLiteral("podman"), m.captured(1).left(12), {}};

    // --- LXD (systemd-managed) -------------------------------------------
    // Real path:  /system.slice/lxd-<name>.service/lxc.payload
    // Match the lxd-NAME.service segment; tested before plain systemd
    // unit fallback so LXD wins over generic "unit:lxd-foo.service".
    static const QRegularExpression rxLxd(
        QStringLiteral("/lxd-([^/\\s.]+)\\.service"));
    if (const auto m = rxLxd.match(path); m.hasMatch())
        return ContainerInfo{QStringLiteral("lxd"), m.captured(1), {}};

    // --- LXC --------------------------------------------------------------
    static const QRegularExpression rxLxc(
        QStringLiteral("/lxc(?:\\.payload)?[./]([^/\\s]+)"));
    if (const auto m = rxLxc.match(path); m.hasMatch())
        return ContainerInfo{QStringLiteral("lxc"), m.captured(1), {}};

    // --- systemd unit (non-container scope) ------------------------------
    // Last path component matters: "/system.slice/foo.service" -> "foo.service".
    // We deliberately exclude bare slices ("user.slice") and the synthetic
    // session scopes ("session-N.scope") — those are not interesting for
    // attribution. Per-user managed services ARE interesting though.
    static const QRegularExpression rxSystemd(
        QStringLiteral("/([A-Za-z0-9@_.\\-]+\\.(?:service|socket|mount))(?:/|$)"));
    if (const auto m = rxSystemd.match(path); m.hasMatch())
        return ContainerInfo{QStringLiteral("systemd"),
                             QStringLiteral("unit:") + m.captured(1), {}};

    return std::nullopt;
}

// Convenience: combine extractPath + classifyPath on raw file contents.
[[nodiscard]] inline std::optional<ContainerInfo>
classifyProcCgroup(QStringView contents)
{
    const QString path = extractPath(contents);
    return classifyPath(path);
}

} // namespace qiftop::backend::cgroup
