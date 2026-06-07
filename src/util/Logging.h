#pragma once

#include <QLoggingCategory>

// Single category for opt-in verbose tracing. Toggled by `--verbose` on the
// command line (see main.cpp); equivalent to setting
// `QT_LOGGING_RULES="qiftop.verbose=true"`.
Q_DECLARE_LOGGING_CATEGORY(lcVerbose)

namespace util::logging {

void setVerbose(bool on);
[[nodiscard]] bool isVerbose();

} // namespace util::logging
