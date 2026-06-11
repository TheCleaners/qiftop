// Sanity tests for the /proc/<pid>/fd/<n> readlink parser. This is the
// only piece of SockDiagResolver's logic that's testable without a live
// netlink dump + /proc walk. The kernel's link format hasn't changed in
// decades but pinning it down catches accidental refactor breakage.

#include <QTest>

#include "backend/linux/SockDiagParse.h"
#include "backend/linux/SockDiagDump.h"

#include <cerrno>
#include <cstring>

#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>

namespace {

// Append one netlink message with the given type and payload to buf,
// letting the caller lie about nlmsg_len (for truncation tests).
void appendNlMsg(QByteArray &buf, quint16 type,
                 const void *payload, quint32 payloadLen,
                 int lieAboutLen = -1)
{
    nlmsghdr nh{};
    nh.nlmsg_len  = (lieAboutLen >= 0) ? quint32(lieAboutLen)
                                       : quint32(NLMSG_LENGTH(payloadLen));
    nh.nlmsg_type = type;
    const int start = buf.size();
    buf.append(reinterpret_cast<const char*>(&nh), sizeof(nh));
    if (payload && payloadLen)
        buf.append(reinterpret_cast<const char*>(payload), payloadLen);
    // NLMSG_ALIGN padding so a following message parses correctly.
    while ((buf.size() - start) % NLMSG_ALIGNTO != 0) buf.append('\0');
}

QByteArray makeDiagMsg(quint32 inode,
                       quint32 srcBe, quint16 sportHost,
                       quint32 dstBe, quint16 dportHost)
{
    inet_diag_msg m{};
    m.idiag_family   = AF_INET;
    m.idiag_inode    = inode;
    m.id.idiag_src[0] = srcBe;
    m.id.idiag_dst[0] = dstBe;
    m.id.idiag_sport  = qToBigEndian(sportHost);
    m.id.idiag_dport  = qToBigEndian(dportHost);
    QByteArray buf;
    appendNlMsg(buf, SOCK_DIAG_BY_FAMILY, &m, sizeof(m));
    return buf;
}

} // namespace

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

    // ------------------------------------------------------------------
    // parseDumpChunk — pure buffer-level netlink dump parsing (M10).
    // ------------------------------------------------------------------

    void dumpChunkParsesValidDiagMsg()
    {
        using namespace qiftop::backend::sockdiag;
        const quint32 src = qToBigEndian(quint32(0x0A000005)); // 10.0.0.5
        const quint32 dst = qToBigEndian(quint32(0xCB007107)); // 203.0.113.7
        const QByteArray buf = makeDiagMsg(4242, src, 8080, dst, 443);

        QHash<QByteArray, quint64> out;
        QCOMPARE(parseDumpChunk(buf.constData(), buf.size(), IPPROTO_TCP, out),
                 DumpChunkResult::NeedMore);
        // Each socket is indexed twice: by full 4-tuple and by local 2-tuple
        // (so unconnected UDP sockets / listeners match a flow by local end).
        QCOMPARE(out.size(), 2);
        const QByteArray key = makeFlowKey(
            IPPROTO_TCP,
            QHostAddress(QStringLiteral("10.0.0.5")),    8080,
            QHostAddress(QStringLiteral("203.0.113.7")), 443);
        QCOMPARE(out.value(key, 0), quint64(4242));
        const QByteArray localKey = makeLocalKey(
            IPPROTO_TCP, QHostAddress(QStringLiteral("10.0.0.5")), 8080);
        QCOMPARE(out.value(localKey, 0), quint64(4242));
        QVERIFY(key != localKey);
    }

    void dumpChunkDoneAndAck()
    {
        using namespace qiftop::backend::sockdiag;
        QHash<QByteArray, quint64> out;

        QByteArray done;
        appendNlMsg(done, NLMSG_DONE, nullptr, 0);
        QCOMPARE(parseDumpChunk(done.constData(), done.size(), IPPROTO_TCP, out),
                 DumpChunkResult::Done);

        nlmsgerr ack{};
        ack.error = 0;
        QByteArray ackBuf;
        appendNlMsg(ackBuf, NLMSG_ERROR, &ack, sizeof(ack));
        QCOMPARE(parseDumpChunk(ackBuf.constData(), ackBuf.size(), IPPROTO_TCP, out),
                 DumpChunkResult::Done);
    }

    void dumpChunkReportsNlmsgError()
    {
        using namespace qiftop::backend::sockdiag;
        nlmsgerr err{};
        err.error = -EPERM;
        QByteArray buf;
        appendNlMsg(buf, NLMSG_ERROR, &err, sizeof(err));

        QHash<QByteArray, quint64> out;
        int nlErrno = 0;
        QCOMPARE(parseDumpChunk(buf.constData(), buf.size(), IPPROTO_TCP,
                                out, &nlErrno),
                 DumpChunkResult::Failed);
        QCOMPARE(nlErrno, int(EPERM));
        QVERIFY(out.isEmpty());
    }

    // M10: an NLMSG_ERROR whose nlmsg_len claims header-only (no
    // nlmsgerr payload behind it) must be rejected WITHOUT reading
    // past the message — previously this dereferenced 4 bytes beyond
    // a short datagram.
    void dumpChunkRejectsTruncatedNlmsgError()
    {
        using namespace qiftop::backend::sockdiag;
        QByteArray buf;
        // nlmsg_len = bare header (16) — valid for NLMSG_OK, but too
        // short to hold a struct nlmsgerr.
        appendNlMsg(buf, NLMSG_ERROR, nullptr, 0,
                    /*lieAboutLen=*/int(NLMSG_LENGTH(0)));

        QHash<QByteArray, quint64> out;
        int nlErrno = 42;
        QCOMPARE(parseDumpChunk(buf.constData(), buf.size(), IPPROTO_TCP,
                                out, &nlErrno),
                 DumpChunkResult::Failed);
        QCOMPARE(nlErrno, 0);   // malformed, not a kernel errno
        QVERIFY(out.isEmpty());
    }

    // A buffer shorter than one nlmsghdr must parse to "need more
    // data" with no entries — never an over-read.
    void dumpChunkToleratesShortBuffer()
    {
        using namespace qiftop::backend::sockdiag;
        const char junk[4] = { 1, 2, 3, 4 };
        QHash<QByteArray, quint64> out;
        QCOMPARE(parseDumpChunk(junk, sizeof(junk), IPPROTO_TCP, out),
                 DumpChunkResult::NeedMore);
        QVERIFY(out.isEmpty());

        QCOMPARE(parseDumpChunk(junk, 0, IPPROTO_TCP, out),
                 DumpChunkResult::NeedMore);
        QVERIFY(out.isEmpty());
    }

    // A SOCK_DIAG_BY_FAMILY message shorter than inet_diag_msg is
    // skipped, not dereferenced.
    void dumpChunkSkipsShortDiagMsg()
    {
        using namespace qiftop::backend::sockdiag;
        const char tiny[4] = { 0, 0, 0, 0 };
        QByteArray buf;
        appendNlMsg(buf, SOCK_DIAG_BY_FAMILY, tiny, sizeof(tiny));
        QHash<QByteArray, quint64> out;
        QCOMPARE(parseDumpChunk(buf.constData(), buf.size(), IPPROTO_TCP, out),
                 DumpChunkResult::NeedMore);
        QVERIFY(out.isEmpty());
    }

    // M9: once the map holds kMaxSocketEntries, further sockets are
    // dropped (hard cap, bounded memory under a socket flood).
    void dumpChunkHonoursEntryCap()
    {
        using namespace qiftop::backend::sockdiag;
        QHash<QByteArray, quint64> out;
        out.reserve(kMaxSocketEntries);
        for (int i = 0; i < kMaxSocketEntries; ++i)
            out.insert(QByteArrayLiteral("pad") + QByteArray::number(i), 1);

        const quint32 src = qToBigEndian(quint32(0x0A000005));
        const quint32 dst = qToBigEndian(quint32(0xCB007107));
        const QByteArray buf = makeDiagMsg(4242, src, 8080, dst, 443);
        QCOMPARE(parseDumpChunk(buf.constData(), buf.size(), IPPROTO_TCP, out),
                 DumpChunkResult::NeedMore);
        QCOMPARE(out.size(), kMaxSocketEntries);   // nothing added past the cap
    }
};

QTEST_GUILESS_MAIN(TestSockDiagParse)
#include "test_sockdiag_parse.moc"
