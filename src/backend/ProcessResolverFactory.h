#pragma once

#include <memory>

#include "ProcessResolver.h"

namespace qiftop::backend {

// Configuration knobs threaded through from the agent's runtime config
// (typically /etc/qiftop/agent.conf, [attribution] section — added in a
// later step). Defaults match "everything that was compiled in, on".
//
// Each field can override a compile-time-enabled feature OFF at runtime
// without recompiling; conversely, no field can turn ON a feature that
// wasn't compiled in (the corresponding source files aren't linked).
struct ProcessResolverConfig {
    bool processAttribution   = true;
    bool containerAttribution = true;
    bool netnsScan            = true;
};

// Construct the best ProcessResolver available on this platform / build.
//
// The factory walks the compile-time enabled features and the runtime
// probe results, returning:
//   * A composed Linux resolver (sock_diag + cgroup + netns layers as
//     enabled) when any QIFTOP_ENABLE_* option is on AND its runtime
//     probe succeeded.
//   * A NullProcessResolver in every other case (non-Linux, all options
//     off, or every layer failed its probe).
//
// The returned resolver has had initialize() called and reports its
// runtime-detected capabilities via capabilities(). Never returns null.
[[nodiscard]] std::unique_ptr<ProcessResolver>
    createProcessResolver(const ProcessResolverConfig &cfg = {});

} // namespace qiftop::backend
