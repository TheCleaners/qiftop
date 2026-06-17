#include "NetlinkMonitor.h"
#include "NetlinkWorker.h"

NetlinkMonitor::NetlinkMonitor(QObject *parent)
    : NetworkMonitor(parent)
    , m_worker(new NetlinkWorker) // no parent; ownership transferred to thread
{
    m_worker->moveToThread(&m_thread);

    // Worker lifetime: start on thread start, clean up on thread finish
    connect(&m_thread, &QThread::started,  m_worker, &NetlinkWorker::start);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    // Forward stats from worker thread to main thread (queued automatically)
    connect(m_worker, &NetlinkWorker::statsUpdated,
            this,     &NetworkMonitor::statsUpdated);
}

NetlinkMonitor::~NetlinkMonitor()
{
    NetlinkMonitor::stop();
}

void NetlinkMonitor::start()
{
    m_thread.start();
}

void NetlinkMonitor::stop()
{
    if (!m_thread.isRunning())
        return;

    // Ask worker to clean up from its own thread, then wait
    QMetaObject::invokeMethod(m_worker, &NetlinkWorker::stop, Qt::QueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void NetlinkMonitor::setPollIntervalMs(int ms)
{
    if (!m_thread.isRunning())
        return;
    QMetaObject::invokeMethod(m_worker, "setPollIntervalMs",
                              Qt::QueuedConnection, Q_ARG(int, ms));
}
