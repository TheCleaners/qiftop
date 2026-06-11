#include "BsdConnectionMonitor.h"

namespace qiftop::backend::bsd {

BsdConnectionMonitor::BsdConnectionMonitor(QObject *parent)
    : ConnectionMonitor(parent)
{}

BsdConnectionMonitor::~BsdConnectionMonitor() = default;

void BsdConnectionMonitor::start()
{
    if (!m_warned) {
        m_warned = true;
        // Defer the signal so listeners connected after construction
        // still receive it.
        QMetaObject::invokeMethod(this, [this] {
            emit accountingUnavailable(
                QStringLiteral("Per-flow accounting is not yet implemented on "
                               "this platform (no conntrack equivalent; a pf/BPF "
                               "backend is the planned datapath)."));
            emit connectionsUpdated({});
        }, Qt::QueuedConnection);
    }
}

void BsdConnectionMonitor::stop()
{
}

} // namespace qiftop::backend::bsd
