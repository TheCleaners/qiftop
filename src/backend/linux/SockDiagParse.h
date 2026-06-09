#pragma once

#include <QString>

#include <optional>

// Pure-logic helpers split out of SockDiagResolver.cpp so they can be
// unit-tested without a live netlink socket / running /proc.

namespace qiftop::backend::sockdiag {

// Parse the target of a /proc/<pid>/fd/<n> readlink.
//
// Kernel formats socket fds as "socket:[<inode>]". Returns the inode
// number on success, std::nullopt for anything else (regular file,
// pipe, anon_inode, malformed, empty).
//
// Examples:
//   "socket:[123456]"      -> 123456
//   "socket:[0]"           -> 0
//   "pipe:[42]"            -> nullopt
//   "/etc/passwd"          -> nullopt
//   "socket:[abc]"         -> nullopt (non-decimal)
//   "socket:[123"          -> nullopt (no closing bracket)
[[nodiscard]] inline std::optional<quint64> parseSocketLink(QStringView link)
{
    constexpr QStringView kPrefix = u"socket:[";
    if (!link.startsWith(kPrefix)) return std::nullopt;
    if (!link.endsWith(u']'))      return std::nullopt;
    const auto inner = link.mid(kPrefix.size(), link.size() - kPrefix.size() - 1);
    if (inner.isEmpty()) return std::nullopt;
    bool ok = false;
    const quint64 inode = inner.toULongLong(&ok, 10);
    return ok ? std::optional<quint64>(inode) : std::nullopt;
}

// Parse the target of a /proc/<pid>/ns/<type> readlink. Kernel formats
// these as e.g. "net:[4026531840]" / "mnt:[4026531841]" / "pid:[...]".
// `type` is the leading token without the colon (e.g. "net").
//
//   parseNamespaceLink(u"net:[4026531840]", "net") -> 4026531840
//   parseNamespaceLink(u"mnt:[1]",          "net") -> nullopt  (wrong type)
//   parseNamespaceLink(u"net:[abc]",        "net") -> nullopt
[[nodiscard]] inline std::optional<quint64>
parseNamespaceLink(QStringView link, QLatin1StringView type)
{
    if (!link.startsWith(type)) return std::nullopt;
    if (link.size() < qsizetype(type.size() + 3)) return std::nullopt;
    if (link[type.size()]     != u':') return std::nullopt;
    if (link[type.size() + 1] != u'[') return std::nullopt;
    if (!link.endsWith(u']'))          return std::nullopt;
    const auto inner = link.mid(type.size() + 2,
                                link.size() - type.size() - 3);
    if (inner.isEmpty()) return std::nullopt;
    bool ok = false;
    const quint64 v = inner.toULongLong(&ok, 10);
    return ok ? std::optional<quint64>(v) : std::nullopt;
}

} // namespace qiftop::backend::sockdiag
