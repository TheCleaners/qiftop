#pragma once

#include <QObject>
#include <QTimer>

#include "backend/NetworkMonitor.h"

struct nl_sock;
struct nl_cache;

// Runs inside the worker thread owned by NetlinkMonitor.
// Manages the libnl-3 socket/cache lifecycle and emits
// statsUpdated on each poll tick.
class NetlinkWorker : public QObject {
    Q_OBJECT

public:
    explicit NetlinkWorker(QObject *parent = nullptr);
    ~NetlinkWorker() override;

public slots:
    void start();
    void stop();
    void setPollIntervalMs(int ms);

signals:
    void statsUpdated(QList<InterfaceStats> stats);

private slots:
    void poll();

private:
    nl_sock  *m_sock      = nullptr;
    nl_cache *m_linkCache = nullptr;
    nl_cache *m_addrCache = nullptr;
    QTimer   *m_timer     = nullptr;
};
