#pragma once

// Mini expression language for filtering Connections rows.
//
// Grammar (informal, case-insensitive keywords):
//
//   expr     := orExpr
//   orExpr   := andExpr ( ("or"|"||")  andExpr )*
//   andExpr  := notExpr ( ("and"|"&&") notExpr )*
//   notExpr  := ("not"|"!") notExpr | atom
//   atom     := "(" expr ")" | predicate
//   predicate := field op value
//   op       := ":" | "=" | "!=" | "~" | "<" | "<=" | ">" | ">="
//   field    := proto | src | dst | host | sport | dport | port
//             | iface | family | direction
//             | bytes_in | bytes_out | bytes
//             | pkts_in  | pkts_out  | pkts
//             | rate_in  | rate_out  | rate
//             | pid | uid | comm | runtime | container | chain_has
//
// Semantics:
//   "field:val"   string field: case-insensitive substring; numeric field: ==
//   "field=val"   same as ":" for strings (exact, case-insensitive) /
//                 == for numerics
//   "field!=val"  negation of "="
//   "field~rx"    regex match (QRegularExpression, case-insensitive)
//   "<,<=,>,>="   numeric comparison only
//
// Numeric values accept byte suffixes: K/M/G/T (decimal, ×1000) and
// Ki/Mi/Gi/Ti (binary, ×1024). "1M" == 1000000; "1Mi" == 1048576.
//
// Pure-logic; no Qt model/view dependency. Lives in util/ so tests can
// exercise it without instantiating the proxy.

#include <QRegularExpression>
#include <QString>

#include <memory>
#include <variant>

#include "backend/Connection.h"

namespace qiftop::filter {

// All inputs needed to evaluate a filter against a single row.
struct Context {
    const Connection &c;
    double  rxRate = 0.0;   // bytes/s, smoothed (display) — same value user sees
    double  txRate = 0.0;
    QString hostnameLocal;  // resolved local hostname, or empty
    QString hostnameRemote; // resolved remote hostname, or empty
};

// Opaque parsed-expression node. The .cpp owns the AST; callers hold a
// shared_ptr and ask matches() per row.
class Expr;
using ExprPtr = std::shared_ptr<const Expr>;

// Parse result: either an AST or a human-readable error string.
struct ParseResult {
    ExprPtr expr;            // null on error
    QString error;           // empty on success
    int     errorPos = -1;   // 0-based column in input
};

// Parse an expression. Empty input yields a success result with a
// null expr — callers should treat null expr as "match-all" (i.e.
// filter is disabled).
[[nodiscard]] ParseResult parse(const QString &input);

// Evaluate. Null expr ⇒ true (no filter).
[[nodiscard]] bool matches(const ExprPtr &expr, const Context &ctx);

// One-shot helpers for tests / convenience.
[[nodiscard]] inline bool matches(const QString &input, const Context &ctx)
{
    return matches(parse(input).expr, ctx);
}

// Human-readable syntax cheat sheet (rich text). Used by the UI "?"
// button. Kept here so the syntax docs live next to the grammar.
[[nodiscard]] QString helpHtml();

} // namespace qiftop::filter
