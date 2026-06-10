#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>

#include "backend/NetworkMonitor.h"

namespace qiftop::aggregate {

// Plain-QObject aggregator for per-interface stats. Owns the rate
// computation (delta bytes / delta time) and the sorted row set so that
// EVERY frontend (the Qt GUI's NetworkModel, the ncurses nqiftop, a future
// exporter) shares one implementation instead of re-deriving it. It has NO
// QAbstractItemModel / Widgets dependency — it lives in libqiftop and emits
// coarse change signals that a model adapter translates into begin/endReset
// + dataChanged.
class InterfaceAggregator : public QObject {
    Q_OBJECT

public:
    struct Row {
        InterfaceStats current{};
        double rxRate = 0.0; // bytes/sec
        double txRate = 0.0; // bytes/sec
    };

    explicit InterfaceAggregator(QObject *parent = nullptr);

    [[nodiscard]] int               rowCount() const { return static_cast<int>(m_rows.size()); }
    [[nodiscard]] const QList<Row> &rows() const { return m_rows; }
    [[nodiscard]] const Row        &rowAt(int i) const { return m_rows[i]; }

public slots:
    void updateStats(QList<InterfaceStats> stats);

signals:
    // Emitted around an add/remove of interfaces (row set identity changed).
    // A model adapter maps these to beginResetModel()/endResetModel().
    void aboutToReset();
    void didReset();
    // Emitted when existing rows' values changed in place (rate/total refresh)
    // without the row set changing. Maps to dataChanged() over [first,last].
    void rowsChanged(int first, int last);

private:
    QList<Row>                     m_rows; // sorted by name
    QHash<QString, InterfaceStats> m_prev;
    QElapsedTimer                  m_elapsed;
    qint64                         m_lastElapsedMs = 0;
};

} // namespace qiftop::aggregate
