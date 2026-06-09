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

} // namespace qiftop::backend::sockdiag
