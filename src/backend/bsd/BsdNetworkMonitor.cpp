#include "BsdNetworkMonitor.h"
#include "BsdNetworkWorker.h"

namespace qiftop::backend::bsd {

BsdNetworkMonitor::BsdNetworkMonitor(QObject *parent)
    : NetworkMonitor(parent)
    , m_worker(new BsdNetworkWorker) // no parent; ownership transferred to thread
{
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::started,  m_worker, &BsdNetworkWorker::start);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &BsdNetworkWorker::statsUpdated,
            this,     &NetworkMonitor::statsUpdated);
}

BsdNetworkMonitor::~BsdNetworkMonitor()
{
    stop();
}

void BsdNetworkMonitor::start()
{
    m_thread.start();
}

void BsdNetworkMonitor::stop()
{
    if (!m_thread.isRunning())
        return;

    QMetaObject::invokeMethod(m_worker, &BsdNetworkWorker::stop, Qt::QueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void BsdNetworkMonitor::setPollIntervalMs(int ms)
{
    if (!m_thread.isRunning())
        return;
    QMetaObject::invokeMethod(m_worker, "setPollIntervalMs",
                              Qt::QueuedConnection, Q_ARG(int, ms));
}

} // namespace qiftop::backend::bsd
