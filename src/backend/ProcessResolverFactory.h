#pragma once

#include <memory>

#include "ProcessResolver.h"

namespace qiftop::backend {

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
