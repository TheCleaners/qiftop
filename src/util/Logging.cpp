#include "Logging.h"

Q_LOGGING_CATEGORY(lcVerbose, "qiftop.verbose", QtWarningMsg)

namespace util::logging {

namespace {
bool g_verbose = false;
}

void setVerbose(bool on)
{
    g_verbose = on;
    QLoggingCategory::setFilterRules(on ? QStringLiteral("qiftop.verbose=true")
                                        : QStringLiteral("qiftop.verbose=false"));
}

bool isVerbose() { return g_verbose; }

} // namespace util::logging
