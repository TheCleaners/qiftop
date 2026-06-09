#pragma once

#include "backend/ProcessResolver.h"

namespace qiftop::backend {

// Universal no-op ProcessResolver. Used as the fallback whenever:
//   * The platform has no attribution backend (anything not Linux today).
//   * All three QIFTOP_ENABLE_* compile-time options are OFF.
//   * The Linux runtime probe failed (e.g. ancient kernel without
//     NETLINK_SOCK_DIAG); the factory degrades to this implementation
//     rather than refusing to start.
//
// capabilities() is intentionally empty so clients see no
// process/container tokens on the DBus surface and hide the UI rows
// per the "missing = absent, not greyed out" rule.
class NullProcessResolver final : public ProcessResolver {
public:
    bool initialize() override { return true; }

    [[nodiscard]] QStringList capabilities() const override { return {}; }

    [[nodiscard]] qint32 resolvePid(const Connection &) override { return 0; }

    [[nodiscard]] std::optional<ProcessInfo>
        enrichPid(qint32) override { return std::nullopt; }

    [[nodiscard]] std::optional<ContainerInfo>
        resolveContainerForPid(qint32) override { return std::nullopt; }
};

} // namespace qiftop::backend
