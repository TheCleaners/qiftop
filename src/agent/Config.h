#pragma once

#include <QString>
#include <QStringList>

#include "backend/ProcessResolver.h"
#include "IdleManager.h"

namespace qiftop::agent {

// Default config file path. Override with `qiftop-agent --config <path>`.
constexpr auto kDefaultConfigPath = "/etc/qiftop/agent.conf";

// Loads the IdleManager config from an INI file at `path` (see
// dist/conf/agent.conf for documentation of every key). Missing file
// is not an error — built-in defaults are returned. Out-of-range
// numeric values are clamped to the documented bounds and warned about
// on stderr.
//
// Extracted from agent/main.cpp so it can be unit-tested without
// spawning the agent.
[[nodiscard]] IdleManager::Config loadIdleConfig(const QString &path);

// Runtime attribution knobs from the `[attribution]` section. The loaded
// values are already resolved through precedence rules: eagerness preset,
// per-feature toggles, and advanced refresh overrides.
using AttributionConfig = qiftop::backend::ProcessResolverConfig;

[[nodiscard]] AttributionConfig loadAttributionConfig(const QString &path);

// Policy controlling who may see the *privileged* per-process detail
// fields (exe / cwd / cmdline) returned by Connections.GetProcessDetails.
// The low-sensitivity bulk fields (pid / uid / comm / startTime) are
// always returned to any authorised (netdev-group) caller — this policy
// only gates the fields the root agent can read across UID boundaries
// thanks to CAP_SYS_PTRACE / CAP_DAC_READ_SEARCH.
struct ProcessDetailsPolicy {
    enum class Mode {
        Owner,       // root or the process owner (default — least disclosure)
        Permissive,  // any authorised caller (pre-0.2.1 behaviour)
        Restricted,  // root, the owner, or a caller in allowUsers / allowGroups
    };
    Mode        mode = Mode::Owner;
    QStringList allowUsers;   // usernames granted cross-UID detail (Restricted)
    QStringList allowGroups;  // group names granted cross-UID detail (Restricted)
};

// Parse the `[process_details]` section of the INI file at `path`.
// Missing file / section yields the default (Owner) policy. An
// unrecognised `disclosure` value warns and falls back to Owner.
[[nodiscard]] ProcessDetailsPolicy loadProcessDetailsPolicy(const QString &path);

} // namespace qiftop::agent
