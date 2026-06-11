#include "BsdConnectionMonitor.h"
#include "BsdConnectionWorker.h"

namespace qiftop::backend::bsd {

BsdConnectionMonitor::BsdConnectionMonitor(QObject *parent)
    : ConnectionMonitor(parent)
    , m_worker(new BsdConnectionWorker) // no parent; moved to m_thread
{
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::started,  m_worker, &BsdConnectionWorker::start);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &BsdConnectionWorker::connectionsUpdated,
            this,     &ConnectionMonitor::connectionsUpdated);
    connect(m_worker, &BsdConnectionWorker::accountingUnavailable,
            this,     &ConnectionMonitor::accountingUnavailable);
}

BsdConnectionMonitor::~BsdConnectionMonitor()
{
    stop();
}

void BsdConnectionMonitor::start()
{
    m_thread.start();
}

void BsdConnectionMonitor::stop()
{
    if (!m_thread.isRunning())
        return;
    QMetaObject::invokeMethod(m_worker, &BsdConnectionWorker::stop, Qt::QueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void BsdConnectionMonitor::setPollIntervalMs(int ms)
{
    if (!m_thread.isRunning())
        return;
    QMetaObject::invokeMethod(m_worker, "setPollIntervalMs",
                              Qt::QueuedConnection, Q_ARG(int, ms));
}

} // namespace qiftop::backend::bsd
