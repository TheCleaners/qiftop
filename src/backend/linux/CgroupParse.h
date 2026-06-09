#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtGlobal>

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

// Maximum number of nested container scopes we'll record from a single
// cgroup path. Real-world paths cap out around 8 segments even for
// k3s-in-docker; 16 is a paranoia bound. If a path classifies more
// segments than this we keep the INNERMOST 16 (the outer ones get
// truncated, with a qWarning) — the leaf is always the most relevant.
inline constexpr int kMaxContainerChainDepth = 16;

// Classify EVERY container-meaningful segment in `path`, returning the
// full nesting chain ordered OUTERMOST → INNERMOST. The last element
// (when non-empty) is the actual container the process lives in; any
// preceding elements describe the wrappers around it (e.g. for a pod
// inside a k3d node: [docker:node, kubernetes:pod, containerd:workload]).
//
// Empty / host-scope paths return an empty list. The systemd-unit
// fallback contributes at most a single entry and only when no
// container scope was found anywhere in the path — host-level units
// shouldn't pretend to be containers, but a process scoped under
// nginx.service with no container wrapper deserves a visible unit
// label.
//
// Patterns recognised (lowercase runtime token, 12-char short id):
//   docker            "/docker/<64hex>" or "docker-<64hex>.scope"
//   containerd / k8s  "cri-containerd-<64hex>.scope", "kubepods/.../<hex>"
//   podman            "libpod-<64hex>.scope"
//   lxc / lxd         "/lxc/<name>", "lxc.payload.<name>/"
//   systemd           "*.service" / "*.slice" outside of /user.slice
//                     (kept distinct from host so the UI can show e.g. "unit:nginx.service";
//                     /user.slice is explicitly excluded so a desktop's user@<uid>.service
//                     and its app children are NOT misclassified as systemd units)
[[nodiscard]] inline QList<ContainerInfo> classifyPathChain(const QString &path)
{
    QList<ContainerInfo> chain;
    if (path.isEmpty() || path == QLatin1String("/")
        || path == QLatin1String("/init.scope")) {
        return chain;
    }

    // Patterns recognised on a SINGLE path segment. We walk segments
    // from ROOT to LEAF and append each classifiable segment to the
    // chain in order — the result is outer-to-inner, matching how a
    // human reads "/system.slice/docker-X.scope/.../cri-containerd-Y.scope".
    //
    // Order within a single segment is "specificity, then alphabetical":
    // runtime-specific scopes before the generic kubepods fallback,
    // kubepods fallback before the systemd-unit fallback.
    static const QRegularExpression rxCriContainerd(
        QStringLiteral("^cri-containerd-([0-9a-f]{64})\\.scope$"));
    static const QRegularExpression rxCrio(
        QStringLiteral("^crio-([0-9a-f]{64})\\.scope$"));
    static const QRegularExpression rxDockerScope(
        QStringLiteral("^docker-([0-9a-f]{64})\\.scope$"));
    static const QRegularExpression rxPodman(
        QStringLiteral("^libpod-([0-9a-f]{64})\\.scope$"));
    // Podman cgroupfs cgroup manager (older cgroup-v1 hosts and any
    // host explicitly configured with `--cgroup-manager cgroupfs`):
    // the per-container segment is `libpod-<64hex>` WITHOUT a `.scope`
    // suffix. Without this branch the unanchored rxKubepodsPod below
    // matches `pod-<63hex>` inside `libpod-<64hex>` at offset 3 and
    // mis-classifies the flow as `kubernetes` with a garbled id. Must
    // be checked BEFORE rxKubepodsPod for that reason.
    static const QRegularExpression rxPodmanCgfs(
        QStringLiteral("^libpod-([0-9a-f]{64})$"));
    static const QRegularExpression rxLxd(
        QStringLiteral("^lxd-([^/\\s.]+)\\.service$"));
    // systemd-nspawn: containers registered via systemd-machined live
    // under /machine.slice/machine-<NAME>.scope. NAME may include
    // systemd-style hex escapes (\x2d for '-' etc) — we capture
    // verbatim; users see the same escaped form in `machinectl list`.
    //
    // Caveat: libvirt VMs (when libvirtd is built with machined
    // integration) also register here as machine-qemu\x2d<id>.scope.
    // That's a known mislabel — qiftop will call them runtime=nspawn
    // even though they're actually QEMU. The machine NAME usually
    // makes it obvious in the UI. Fixing this would require parsing
    // sd-bus GetMachineByPID, which is more plumbing than the
    // mislabel is worth.
    static const QRegularExpression rxNspawnScope(
        QStringLiteral("^machine-(.+)\\.scope$"));
    // Templated systemd unit form: `systemctl start systemd-nspawn@NAME`
    // produces cgroup segment `systemd-nspawn@NAME.service`. Less
    // common than the machinectl path but real (used by sysadmins
    // who want auto-restart semantics).
    static const QRegularExpression rxNspawnService(
        QStringLiteral("^systemd-nspawn@(.+)\\.service$"));
    // Pod UIDs use `_` under the systemd cgroup driver
    // (kubepods-besteffort-pod665b0949_7b83_....slice) and `-` under
    // the cgroupfs driver (pod665b0949-7b83-...). Allow both.
    static const QRegularExpression rxKubepodsPod(
        QStringLiteral("pod([0-9a-fA-F_-]{32,72})"));
    // Cgroupfs-driver pod segment is the bare "pod<UID>" — no .slice,
    // no kubepods prefix on the same segment. Anchored so it doesn't
    // false-match against random "pod-foo" service names.
    static const QRegularExpression rxKubepodsPodCgfs(
        QStringLiteral("^pod([0-9a-fA-F_-]{32,72})$"));
    // Cgroupfs-driver containerd leaf is a NAKED 64-hex segment with
    // no prefix/suffix. We only treat it as a containerd container
    // when its parent segment was a pod (or another kubernetes-tier
    // segment) — otherwise the pattern is too generic.
    static const QRegularExpression rxBareHex64(
        QStringLiteral("^([0-9a-f]{64})$"));

    // Segment-aware classifier for container-bearing segments only.
    // systemd-unit / lxc-legacy / docker-legacy shapes are handled
    // separately because they aren't single-segment patterns. The
    // cgroupfs-driver bare-hex containerd leaf needs CONTEXT (parent
    // segment must be a pod) so it's also handled below, not here.
    //
    // ID truncation rule: hex-ID runtimes (docker, containerd, cri-o,
    // podman, k8s pod) `.left(12)` to match what `docker ps` etc.
    // display. Name-ID runtimes (lxd, lxc, nspawn) keep the FULL
    // captured name — truncating a human-chosen name would destroy
    // information, not summarise it. When adding a new classifier
    // branch, decide which family it belongs to before reflexively
    // copying `.left(12)`. See docs/ATTRIBUTION.md §5c.
    auto classifySegment = [](const QString &seg) -> std::optional<ContainerInfo> {
        if (const auto m = rxCriContainerd.match(seg); m.hasMatch())
            return ContainerInfo{QStringLiteral("containerd"), m.captured(1).left(12), {}};
        if (const auto m = rxCrio.match(seg); m.hasMatch())
            return ContainerInfo{QStringLiteral("cri-o"), m.captured(1).left(12), {}};
        if (const auto m = rxDockerScope.match(seg); m.hasMatch())
            return ContainerInfo{QStringLiteral("docker"), m.captured(1).left(12), {}};
        if (const auto m = rxPodman.match(seg); m.hasMatch())
            return ContainerInfo{QStringLiteral("podman"), m.captured(1).left(12), {}};
        if (const auto m = rxPodmanCgfs.match(seg); m.hasMatch())
            return ContainerInfo{QStringLiteral("podman"), m.captured(1).left(12), {}};
        if (const auto m = rxLxd.match(seg); m.hasMatch())
            return ContainerInfo{QStringLiteral("lxd"), m.captured(1), {}};
        if (const auto m = rxNspawnScope.match(seg); m.hasMatch())
            return ContainerInfo{QStringLiteral("nspawn"), m.captured(1), {}};
        if (const auto m = rxNspawnService.match(seg); m.hasMatch())
            return ContainerInfo{QStringLiteral("nspawn"), m.captured(1), {}};
        if (const auto m = rxKubepodsPodCgfs.match(seg); m.hasMatch())
            return ContainerInfo{QStringLiteral("kubernetes"), m.captured(1).left(12), {}};
        if (const auto m = rxKubepodsPod.match(seg); m.hasMatch())
            return ContainerInfo{QStringLiteral("kubernetes"), m.captured(1).left(12), {}};
        return std::nullopt;
    };

    const QStringList segments = path.split(u'/', Qt::SkipEmptyParts);
    QString prevRuntime;
    for (const auto &seg : segments) {
        if (auto ci = classifySegment(seg)) {
            chain.append(*ci);
            prevRuntime = ci->runtime;
            continue;
        }
        // Stateful: a bare 64-hex segment IMMEDIATELY following a
        // kubernetes pod scope is the cgroupfs-driver containerd leaf
        // (path shape: ".../kubepods/burstable/pod<UID>/<64hex>"). We
        // gate on prevRuntime == "kubernetes" to avoid false matches
        // on hypothetical bare-hex segments elsewhere in the tree.
        if (prevRuntime == QLatin1String("kubernetes")) {
            if (const auto m = rxBareHex64.match(seg); m.hasMatch()) {
                chain.append(ContainerInfo{
                    QStringLiteral("containerd"), m.captured(1).left(12), {}});
                prevRuntime = QStringLiteral("containerd");
                continue;
            }
        }
        // Non-classifiable segment — reset the leaf-tracking state so
        // a later kubepods → bare-hex pair doesn't accidentally couple
        // across an irrelevant wrapper.
        prevRuntime.clear();
    }

    // --- Legacy / non-systemd shapes ---------------------------------------
    // cgroupfs-driver docker: /docker/<64hex> (no .scope suffix). This is
    // a multi-segment pattern so it lives outside the segment loop above.
    // We still try to position it correctly in the chain by walking
    // path-wise rather than appending blindly to the end.
    static const QRegularExpression rxDockerLegacy(
        QStringLiteral("/docker/([0-9a-f]{64})(?:/|$)"));
    if (chain.isEmpty()) {
        auto it = rxDockerLegacy.globalMatch(path);
        while (it.hasNext()) {
            const auto m = it.next();
            chain.append(ContainerInfo{
                QStringLiteral("docker"), m.captured(1).left(12), {}});
        }
    }

    // LXC (plain, not via systemd): /lxc/NAME or /lxc.payload.NAME/
    static const QRegularExpression rxLxc(
        QStringLiteral("/lxc(?:\\.payload)?[./]([^/\\s]+)"));
    if (chain.isEmpty()) {
        if (const auto m = rxLxc.match(path); m.hasMatch())
            chain.append(ContainerInfo{
                QStringLiteral("lxc"), m.captured(1), {}});
    }

    // --- systemd unit (non-container scope) --------------------------------
    // Only contribute a systemd-unit entry when NOTHING container-shaped
    // matched. A pod whose pause container lives in a *.service slice
    // shouldn't be relabelled as "unit:..." — the kubepods/containerd
    // segments are more informative.
    //
    // ALSO skip the entire /user.slice hierarchy: paths like
    // /user.slice/user-1000.slice/user@1000.service belong to the
    // desktop user's per-user systemd manager. Treating
    // user@<uid>.service as a container would mis-label every browser
    // tab, every helper process, every Wayland client on a desktop
    // host. User-session apps should appear as "(host)" — they are
    // host processes from the agent's perspective, owned by uid<n>.
    // (System-managed services under /system.slice/foo.service stay
    // eligible — that's the operationally interesting case.)
    if (chain.isEmpty() && !segments.contains(QLatin1String("user.slice"))) {
        static const QRegularExpression rxSystemdUnit(
            QStringLiteral("^([A-Za-z0-9@_.\\-]+\\.(?:service|socket|mount))$"));
        for (auto it = segments.crbegin(); it != segments.crend(); ++it) {
            if (const auto m = rxSystemdUnit.match(*it); m.hasMatch()) {
                chain.append(ContainerInfo{
                    QStringLiteral("systemd"),
                    QStringLiteral("unit:") + m.captured(1), {}});
                break;
            }
        }
    }

    // Cap depth: keep the innermost N, discarding outer wrappers. The
    // leaf is the most operationally meaningful part of the chain;
    // running out of room at the root end is acceptable.
    if (chain.size() > kMaxContainerChainDepth) {
        qWarning("qiftop: cgroup path classified %lld segments (max %d) — "
                 "truncating to innermost %d. Path: %s",
                 static_cast<long long>(chain.size()),
                 kMaxContainerChainDepth, kMaxContainerChainDepth,
                 qUtf8Printable(path));
        chain = chain.mid(chain.size() - kMaxContainerChainDepth);
    }

    return chain;
}

// Classify a cgroup path into a SINGLE ContainerInfo — the innermost
// container scope found. Convenience wrapper over `classifyPathChain`
// for call sites that don't (yet) care about nesting. Returns nullopt
// for empty / host-scope paths.
[[nodiscard]] inline std::optional<ContainerInfo> classifyPath(const QString &path)
{
    const auto chain = classifyPathChain(path);
    if (chain.isEmpty()) return std::nullopt;
    return chain.last();
}

// Convenience: combine extractPath + classifyPathChain on raw file
// contents. Returns the full outer→inner chain.
[[nodiscard]] inline QList<ContainerInfo>
classifyProcCgroupChain(QStringView contents)
{
    return classifyPathChain(extractPath(contents));
}

// Convenience: combine extractPath + classifyPath (innermost only) on
// raw file contents.
[[nodiscard]] inline std::optional<ContainerInfo>
classifyProcCgroup(QStringView contents)
{
    return classifyPath(extractPath(contents));
}

} // namespace qiftop::backend::cgroup
