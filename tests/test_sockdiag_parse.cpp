// Sanity tests for the /proc/<pid>/fd/<n> readlink parser. This is the
// only piece of SockDiagResolver's logic that's testable without a live
// netlink dump + /proc walk. The kernel's link format hasn't changed in
// decades but pinning it down catches accidental refactor breakage.

#include <QTest>

#include "backend/linux/SockDiagParse.h"

class TestSockDiagParse : public QObject {
    Q_OBJECT
private slots:
    void parsesValidSocketLink()
    {
        QCOMPARE(qiftop::backend::sockdiag::parseSocketLink(u"socket:[123456]"),
                 std::optional<quint64>(123456));
        QCOMPARE(qiftop::backend::sockdiag::parseSocketLink(u"socket:[0]"),
                 std::optional<quint64>(0));
        QCOMPARE(qiftop::backend::sockdiag::parseSocketLink(u"socket:[18446744073709551610]"),
                 std::optional<quint64>(18446744073709551610ULL));
    }

    void rejectsNonSocketLinks()
    {
        QVERIFY(!qiftop::backend::sockdiag::parseSocketLink(u"pipe:[42]").has_value());
        QVERIFY(!qiftop::backend::sockdiag::parseSocketLink(u"anon_inode:[eventfd]").has_value());
        QVERIFY(!qiftop::backend::sockdiag::parseSocketLink(u"/etc/passwd").has_value());
        QVERIFY(!qiftop::backend::sockdiag::parseSocketLink(u"").has_value());
    }

    void rejectsMalformed()
    {
        QVERIFY(!qiftop::backend::sockdiag::parseSocketLink(u"socket:[]").has_value());
        QVERIFY(!qiftop::backend::sockdiag::parseSocketLink(u"socket:[abc]").has_value());
        QVERIFY(!qiftop::backend::sockdiag::parseSocketLink(u"socket:[123").has_value());
        QVERIFY(!qiftop::backend::sockdiag::parseSocketLink(u"socket:123]").has_value());
        QVERIFY(!qiftop::backend::sockdiag::parseSocketLink(u"socket:[-1]").has_value());
        QVERIFY(!qiftop::backend::sockdiag::parseSocketLink(u"socket:[12 34]").has_value());
    }
};

QTEST_GUILESS_MAIN(TestSockDiagParse)
#include "test_sockdiag_parse.moc"
