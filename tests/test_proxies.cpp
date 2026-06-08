// Unit tests for ConnectionFilterProxy and InterfaceFilterProxy.
// Uses hand-rolled QAbstractTableModel fakes that expose the same
// custom roles the real ConnectionModel / NetworkModel publish, so
// the proxy logic can be exercised without dragging in the heavy
// real models (which depend on Settings, DnsResolver, etc.).

#include <QAbstractTableModel>
#include <QTest>

#include "backend/Connection.h"
#include "ui/ConnectionFilterProxy.h"
#include "ui/ConnectionModel.h"
#include "ui/InterfaceFilterProxy.h"
#include "ui/NetworkModel.h"

namespace {

struct ConnRow {
    QString iface;
    bool    isIPv6 = false;
    L4Proto proto  = L4Proto::Tcp;
    Connection conn;        // used by expression filter
    double  rxRate = 0.0;
    double  txRate = 0.0;
    QString hostLocal;
    QString hostRemote;
};

class FakeConnModel : public QAbstractTableModel {
public:
    QList<ConnRow> rows;
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : rows.size(); }
    int columnCount(const QModelIndex & = {}) const override { return 1; }
    QVariant data(const QModelIndex &i, int role) const override
    {
        if (!i.isValid() || i.row() >= rows.size()) return {};
        const auto &r = rows.at(i.row());
        switch (role) {
        case ConnectionModel::IsIPv6Role:        return r.isIPv6;
        case ConnectionModel::ProtoRole:         return int(r.proto);
        case ConnectionModel::IfaceNameRole:     return r.iface;
        case ConnectionModel::ConnectionRole:    return QVariant::fromValue(r.conn);
        case ConnectionModel::RxRateRole:        return r.rxRate;
        case ConnectionModel::TxRateRole:        return r.txRate;
        case ConnectionModel::HostnameLocalRole: return r.hostLocal;
        case ConnectionModel::HostnameRemoteRole:return r.hostRemote;
        default: return {};
        }
    }
};

struct IfaceRow {
    bool isLoopback = false;
    bool isUp       = true;
};

class FakeIfaceModel : public QAbstractTableModel {
public:
    QList<IfaceRow> rows;
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : rows.size(); }
    int columnCount(const QModelIndex & = {}) const override { return 1; }
    QVariant data(const QModelIndex &i, int role) const override
    {
        if (!i.isValid() || i.row() >= rows.size()) return {};
        const auto &r = rows.at(i.row());
        switch (role) {
        case NetworkModel::IsLoopbackRole: return r.isLoopback;
        case NetworkModel::IsUpRole:       return r.isUp;
        default: return {};
        }
    }
};

} // namespace

class TestProxies : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<Connection>();
    }

    // ---------------- ConnectionFilterProxy ----------------

    void connToggleIPv6FiltersOnlyIPv6Rows()
    {
        FakeConnModel src;
        src.rows = {{.iface = QStringLiteral("eth0"), .isIPv6 = false},
                    {.iface = QStringLiteral("eth0"), .isIPv6 = true},
                    {.iface = QStringLiteral("eth0"), .isIPv6 = false}};
        ConnectionFilterProxy proxy;
        proxy.setSourceModel(&src);
        QCOMPARE(proxy.rowCount(), 3);
        proxy.setShowIPv6(false);
        QCOMPARE(proxy.rowCount(), 2);
        proxy.setShowIPv6(true);
        QCOMPARE(proxy.rowCount(), 3);
    }

    void connTcpUdpTogglesAreIndependent()
    {
        FakeConnModel src;
        src.rows = {{.iface = QStringLiteral("eth0"), .proto = L4Proto::Tcp},
                    {.iface = QStringLiteral("eth0"), .proto = L4Proto::Udp},
                    {.iface = QStringLiteral("eth0"), .proto = L4Proto::Icmp}};
        ConnectionFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setShowTcp(false);
        QCOMPARE(proxy.rowCount(), 2);          // UDP + ICMP
        proxy.setShowUdp(false);
        QCOMPARE(proxy.rowCount(), 1);          // ICMP only
        proxy.setShowTcp(true); proxy.setShowUdp(true);
        QCOMPARE(proxy.rowCount(), 3);
    }

    void connVisibleIfacesEmptySetMeansShowAll()
    {
        FakeConnModel src;
        src.rows = {{.iface = QStringLiteral("eth0")},
                    {.iface = QStringLiteral("wlan0")},
                    {.iface = QStringLiteral("")}};
        ConnectionFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setVisibleIfaces({}); // empty == show all
        QCOMPARE(proxy.rowCount(), 3);
    }

    void connVisibleIfacesFiltersOutOthers()
    {
        FakeConnModel src;
        src.rows = {{.iface = QStringLiteral("eth0")},
                    {.iface = QStringLiteral("wlan0")},
                    {.iface = QStringLiteral("")}};
        ConnectionFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setVisibleIfaces({QStringLiteral("eth0")});
        QCOMPARE(proxy.rowCount(), 1);
    }

    void connEmptyStringSentinelKeepsUnattributedFlows()
    {
        // The toolbar uses the empty-string entry as the "unattributed"
        // checkbox. Selecting iface "eth0" AND the empty sentinel must
        // show eth0 rows AND rows whose iface field is empty, but hide
        // rows attributed to a different iface (e.g. wlan0).
        FakeConnModel src;
        src.rows = {{.iface = QStringLiteral("eth0")},
                    {.iface = QStringLiteral("wlan0")},
                    {.iface = QStringLiteral("")}};
        ConnectionFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setVisibleIfaces({QStringLiteral("eth0"), QString()});
        QCOMPARE(proxy.rowCount(), 2);
    }

    void connFilterExpressionParseErrorIsReported()
    {
        FakeConnModel src;
        src.rows = {{.iface = QStringLiteral("eth0")}};
        ConnectionFilterProxy proxy;
        proxy.setSourceModel(&src);
        const QString err = proxy.setFilterExpression(QStringLiteral("("));
        QVERIFY2(!err.isEmpty(), "incomplete expression must yield a parse error");
        // With a bad expression, the filter falls back to "no filter" so
        // the user can still see what they were filtering against.
        QCOMPARE(proxy.rowCount(), 1);
    }

    void connFilterExpressionEmptyClearsFilter()
    {
        FakeConnModel src;
        src.rows = {{.iface = QStringLiteral("eth0")},
                    {.iface = QStringLiteral("wlan0")}};
        ConnectionFilterProxy proxy;
        proxy.setSourceModel(&src);
        // Expression filters on Connection.iface (not the model's iface
        // field); our fake rows leave that empty so the expression below
        // matches 0 rows. The important property under test is the
        // *clearing* path — empty string must restore unfiltered view.
        const QString err1 = proxy.setFilterExpression(QStringLiteral("iface = \"eth0\""));
        QVERIFY(err1.isEmpty());
        QCOMPARE(proxy.rowCount(), 0);
        const QString err2 = proxy.setFilterExpression(QString());
        QVERIFY(err2.isEmpty());
        QCOMPARE(proxy.rowCount(), 2);
    }

    // ---------------- InterfaceFilterProxy ----------------

    void ifaceLoopbackHiddenByDefault()
    {
        FakeIfaceModel src;
        src.rows = {{.isLoopback = false, .isUp = true},
                    {.isLoopback = true,  .isUp = true},
                    {.isLoopback = false, .isUp = true}};
        InterfaceFilterProxy proxy;
        proxy.setSourceModel(&src);
        QVERIFY(!proxy.showLoopback());
        QCOMPARE(proxy.rowCount(), 2);
        proxy.setShowLoopback(true);
        QCOMPARE(proxy.rowCount(), 3);
    }

    void ifaceDownShownByDefaultButHidableViaToggle()
    {
        FakeIfaceModel src;
        src.rows = {{.isLoopback = false, .isUp = true},
                    {.isLoopback = false, .isUp = false},
                    {.isLoopback = false, .isUp = true}};
        InterfaceFilterProxy proxy;
        proxy.setSourceModel(&src);
        QVERIFY(proxy.showDown());
        QCOMPARE(proxy.rowCount(), 3);
        proxy.setShowDown(false);
        QCOMPARE(proxy.rowCount(), 2);
    }

    void ifaceLoopbackAndDownStackCorrectly()
    {
        // Loopback + down interface: must be hidden by EITHER filter
        // independently; toggling one back doesn't accidentally show
        // the row if the other is still excluding it.
        FakeIfaceModel src;
        src.rows = {{.isLoopback = true, .isUp = false}};
        InterfaceFilterProxy proxy;
        proxy.setSourceModel(&src);
        QCOMPARE(proxy.rowCount(), 0);          // hidden by both
        proxy.setShowLoopback(true);
        QCOMPARE(proxy.rowCount(), 1);          // showing loopback rescues it
        proxy.setShowDown(false);
        QCOMPARE(proxy.rowCount(), 0);          // hiding down kills it again
    }
};

QTEST_MAIN(TestProxies)
#include "test_proxies.moc"
