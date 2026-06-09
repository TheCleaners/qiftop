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

    // /proc/<pid>/ns/<type> links — same shape but typed.
    void parsesValidNamespaceLink()
    {
        using qiftop::backend::sockdiag::parseNamespaceLink;
        QCOMPARE(parseNamespaceLink(u"net:[4026531840]", QLatin1StringView("net")),
                 std::optional<quint64>(4026531840ULL));
        QCOMPARE(parseNamespaceLink(u"mnt:[1]", QLatin1StringView("mnt")),
                 std::optional<quint64>(1));
        QCOMPARE(parseNamespaceLink(u"pid:[4026532256]", QLatin1StringView("pid")),
                 std::optional<quint64>(4026532256ULL));
    }

    void rejectsWrongNamespaceType()
    {
        using qiftop::backend::sockdiag::parseNamespaceLink;
        QVERIFY(!parseNamespaceLink(u"mnt:[1]",        QLatin1StringView("net")).has_value());
        QVERIFY(!parseNamespaceLink(u"netfoo:[1]",     QLatin1StringView("net")).has_value());
        QVERIFY(!parseNamespaceLink(u"net[1]",         QLatin1StringView("net")).has_value());
        QVERIFY(!parseNamespaceLink(u"net:1",          QLatin1StringView("net")).has_value());
        QVERIFY(!parseNamespaceLink(u"net:[abc]",      QLatin1StringView("net")).has_value());
        QVERIFY(!parseNamespaceLink(u"net:[]",         QLatin1StringView("net")).has_value());
        QVERIFY(!parseNamespaceLink(u"net:[123",       QLatin1StringView("net")).has_value());
        QVERIFY(!parseNamespaceLink(u"",               QLatin1StringView("net")).has_value());
    }
};

QTEST_GUILESS_MAIN(TestSockDiagParse)
#include "test_sockdiag_parse.moc"
