#pragma once

#include <QString>

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

} // namespace qiftop::agent
