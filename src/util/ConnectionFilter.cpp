#include "ConnectionFilter.h"

#include <QHostAddress>

#include <cmath>

namespace qiftop::filter {

// ---------- AST ------------------------------------------------------------

enum class Field {
    Proto, Src, Dst, Host, Sport, Dport, Port, Iface, Family, Direction,
    BytesIn, BytesOut, Bytes,
    PktsIn,  PktsOut,  Pkts,
    RateIn,  RateOut,  Rate,
    // v0.4 attribution fields. Numeric: Pid, Uid. String single-haystack:
    // Comm, Runtime. String multi-haystack (like Host): Container (matches
    // runtime+id+name) and ChainHas (any chain entry's runtime+id+name).
    Pid, Uid, Comm, Runtime, Container, ChainHas,
    // v0.5: attribution reason (resolved/forwarded/orphaned/nosocket).
    Reason,
};

enum class Op { Contains, Equals, NotEquals, Regex, Lt, Le, Gt, Ge };

struct Predicate {
    Field            field;
    Op               op;
    QString          textValue;       // raw value, used for strings + regex source
    double           numValue = 0.0;  // parsed numeric (with byte suffix)
    bool             hasNumeric = false;
    QRegularExpression rx;            // compiled iff op==Regex
};

struct Not  { ExprPtr child; };
struct And  { ExprPtr lhs, rhs; };
struct Or   { ExprPtr lhs, rhs; };

class Expr {
public:
    using Node = std::variant<Predicate, Not, And, Or>;
    explicit Expr(Node n) : node(std::move(n)) {}
    Node node;
};

// ---------- Field resolution ----------------------------------------------

namespace {

struct FieldInfo { Field f; bool isNumeric; };

const QHash<QString, FieldInfo> &fieldTable()
{
    static const QHash<QString, FieldInfo> t = {
        { QStringLiteral("proto"),     {Field::Proto,     false} },
        { QStringLiteral("src"),       {Field::Src,       false} },
        { QStringLiteral("dst"),       {Field::Dst,       false} },
        { QStringLiteral("host"),      {Field::Host,      false} },
        { QStringLiteral("iface"),     {Field::Iface,     false} },
        { QStringLiteral("family"),    {Field::Family,    false} },
        { QStringLiteral("direction"), {Field::Direction, false} },
        { QStringLiteral("sport"),     {Field::Sport,     true}  },
        { QStringLiteral("dport"),     {Field::Dport,     true}  },
        { QStringLiteral("port"),      {Field::Port,      true}  },
        { QStringLiteral("bytes_in"),  {Field::BytesIn,   true}  },
        { QStringLiteral("bytes_out"), {Field::BytesOut,  true}  },
        { QStringLiteral("bytes"),     {Field::Bytes,     true}  },
        { QStringLiteral("pkts_in"),   {Field::PktsIn,    true}  },
        { QStringLiteral("pkts_out"),  {Field::PktsOut,   true}  },
        { QStringLiteral("pkts"),      {Field::Pkts,      true}  },
        { QStringLiteral("rate_in"),   {Field::RateIn,    true}  },
        { QStringLiteral("rate_out"),  {Field::RateOut,   true}  },
        { QStringLiteral("rate"),      {Field::Rate,      true}  },
        // v0.4 attribution
        { QStringLiteral("pid"),       {Field::Pid,       true}  },
        { QStringLiteral("uid"),       {Field::Uid,       true}  },
        { QStringLiteral("comm"),      {Field::Comm,      false} },
        { QStringLiteral("runtime"),   {Field::Runtime,   false} },
        { QStringLiteral("container"), {Field::Container, false} },
        { QStringLiteral("chain_has"), {Field::ChainHas,  false} },
        { QStringLiteral("reason"),    {Field::Reason,    false} },
    };
    return t;
}

QString protoName(L4Proto p)
{
    switch (p) {
    case L4Proto::Tcp:    return QStringLiteral("tcp");
    case L4Proto::Udp:    return QStringLiteral("udp");
    case L4Proto::Icmp:   return QStringLiteral("icmp");
    case L4Proto::IcmpV6: return QStringLiteral("icmpv6");
    case L4Proto::Unknown: break;
    }
    return QStringLiteral("?");
}

QString directionName(Direction d)
{
    switch (d) {
    case Direction::Outbound: return QStringLiteral("out");
    case Direction::Inbound:  return QStringLiteral("in");
    case Direction::Unknown:  break;
    }
    return QStringLiteral("?");
}

QString familyName(const Connection &c)
{
    if (c.local.address.protocol() == QAbstractSocket::IPv6Protocol
     || c.remote.address.protocol() == QAbstractSocket::IPv6Protocol)
        return QStringLiteral("v6");
    return QStringLiteral("v4");
}

// Parse a numeric literal with optional byte suffix.
// Returns NaN on parse failure.
double parseNumeric(QString s)
{
    s = s.trimmed();
    if (s.isEmpty()) return std::nan("");
    double mult = 1.0;
    // Order matters: longer suffixes first.
    static const std::pair<const char*, double> suffixes[] = {
        {"KiB", 1024.0}, {"MiB", 1048576.0}, {"GiB", 1073741824.0}, {"TiB", 1099511627776.0},
        {"Ki",  1024.0}, {"Mi",  1048576.0}, {"Gi",  1073741824.0}, {"Ti",  1099511627776.0},
        {"KB",  1000.0}, {"MB",  1000000.0}, {"GB",  1000000000.0}, {"TB",  1000000000000.0},
        {"K",   1000.0}, {"M",   1000000.0}, {"G",   1000000000.0}, {"T",   1000000000000.0},
        {"B",   1.0},
    };
    for (const auto &[suf, m] : suffixes) {
        if (s.endsWith(QLatin1String(suf), Qt::CaseInsensitive)) {
            mult = m;
            s.chop(qstrlen(suf));
            s = s.trimmed();
            break;
        }
    }
    bool ok = false;
    const double n = s.toDouble(&ok);
    if (!ok) return std::nan("");
    return n * mult;
}

// String-form extraction for matching against text predicates.
QString extractText(Field f, const Context &ctx)
{
    const Connection &c = ctx.c;
    switch (f) {
    case Field::Proto:     return protoName(c.proto);
    case Field::Src:       return c.local.address.toString();
    case Field::Dst:       return c.remote.address.toString();
    case Field::Host:      return c.local.address.toString() + QLatin1Char('\n')
                                + c.remote.address.toString() + QLatin1Char('\n')
                                + ctx.hostnameLocal + QLatin1Char('\n')
                                + ctx.hostnameRemote;
    case Field::Iface:     return c.iface;
    case Field::Family:    return familyName(c);
    case Field::Direction: return directionName(c.direction);
    case Field::Comm:      return c.process.comm;
    case Field::Reason:    return attributionReasonToString(c.reason);
    case Field::Runtime:   return c.container.runtime;
    case Field::Container:
        // Multi-haystack: runtime + id + name on separate lines so the
        // multi-line matcher in evalPredicate handles it the same way as
        // Host. Empty when the flow has no container.
        if (c.container.runtime.isEmpty() && c.container.id.isEmpty()
            && c.container.name.isEmpty())
            return {};
        return c.container.runtime + QLatin1Char('\n')
             + c.container.id      + QLatin1Char('\n')
             + c.container.name;
    case Field::ChainHas: {
        // Flatten OUTER→INNER chain into one newline-joined haystack:
        // runtime+id+name for each entry. Multi-line matcher then
        // searches across every ancestor in one pass.
        if (c.containerChain.isEmpty()) return {};
        QString out;
        for (const auto &ci : c.containerChain) {
            if (!out.isEmpty()) out += QLatin1Char('\n');
            out += ci.runtime + QLatin1Char('\n')
                 + ci.id      + QLatin1Char('\n')
                 + ci.name;
        }
        return out;
    }
    default: break;
    }
    return {};
}

double extractNumber(Field f, const Context &ctx)
{
    const Connection &c = ctx.c;
    switch (f) {
    case Field::Sport:    return c.local.port;
    case Field::Dport:    return c.remote.port;
    case Field::Port:     return c.local.port;        // also matched against remote below
    case Field::BytesIn:  return double(c.rxBytes);
    case Field::BytesOut: return double(c.txBytes);
    case Field::Bytes:    return double(c.rxBytes + c.txBytes);
    case Field::PktsIn:   return double(c.rxPackets);
    case Field::PktsOut:  return double(c.txPackets);
    case Field::Pkts:     return double(c.rxPackets + c.txPackets);
    case Field::RateIn:   return ctx.rxRate;
    case Field::RateOut:  return ctx.txRate;
    case Field::Rate:     return ctx.rxRate + ctx.txRate;
    case Field::Pid:      return double(c.process.pid);
    case Field::Uid:      return double(c.process.uid);
    default: break;
    }
    return 0.0;
}

bool evalPredicate(const Predicate &p, const Context &ctx)
{
    // Fields whose extractText returns a newline-joined list of haystacks
    // (any-match semantics for : / = / ~, all-match for !=). Keep this in
    // sync with extractText.
    auto isMultiLineField = [](Field f) {
        return f == Field::Host || f == Field::Container || f == Field::ChainHas;
    };

    if (p.op == Op::Regex) {
        const QString s = extractText(p.field, ctx);
        if (p.field == Field::Port) {
            // "port" is special: numeric. Regex on port = match against
            // both formatted sport and dport.
            const QString sp = QString::number(ctx.c.local.port);
            const QString dp = QString::number(ctx.c.remote.port);
            return p.rx.match(sp).hasMatch() || p.rx.match(dp).hasMatch();
        }
        if (isMultiLineField(p.field)) {
            // Match if any line matches.
            for (const QString &line : s.split(QLatin1Char('\n')))
                if (p.rx.match(line).hasMatch()) return true;
            return false;
        }
        return p.rx.match(s).hasMatch();
    }

    auto isNumericField = [](Field f) {
        switch (f) {
        case Field::Sport: case Field::Dport: case Field::Port:
        case Field::BytesIn: case Field::BytesOut: case Field::Bytes:
        case Field::PktsIn:  case Field::PktsOut:  case Field::Pkts:
        case Field::RateIn:  case Field::RateOut:  case Field::Rate:
        case Field::Pid:     case Field::Uid:
            return true;
        default: return false;
        }
    };

    if (isNumericField(p.field) && p.hasNumeric) {
        if (p.field == Field::Port) {
            // "port:80" or "port=80" → match either end. Order/inequalities
            // also resolve against either end (any-match semantics).
            const double lo = ctx.c.local.port;
            const double ro = ctx.c.remote.port;
            auto cmp = [&](double v) {
                switch (p.op) {
                case Op::Contains: case Op::Equals: return v == p.numValue;
                case Op::NotEquals: return v != p.numValue;
                case Op::Lt: return v <  p.numValue;
                case Op::Le: return v <= p.numValue;
                case Op::Gt: return v >  p.numValue;
                case Op::Ge: return v >= p.numValue;
                default: return false;
                }
            };
            if (p.op == Op::NotEquals)
                return cmp(lo) && cmp(ro);  // neither end equals
            return cmp(lo) || cmp(ro);
        }
        const double v = extractNumber(p.field, ctx);
        switch (p.op) {
        case Op::Contains: case Op::Equals: return v == p.numValue;
        case Op::NotEquals: return v != p.numValue;
        case Op::Lt: return v <  p.numValue;
        case Op::Le: return v <= p.numValue;
        case Op::Gt: return v >  p.numValue;
        case Op::Ge: return v >= p.numValue;
        default: return false;
        }
    }

    // String predicate.
    const QString val = p.textValue;
    if (isMultiLineField(p.field)) {
        // ":"/"=" search across each haystack line (host: both endpoints
        // + both hostnames; container: runtime/id/name; chain_has: every
        // ancestor's runtime/id/name).
        const QString s = extractText(p.field, ctx);
        const auto cmpLine = [&](const QString &line) {
            if (p.op == Op::Equals)    return line.compare(val, Qt::CaseInsensitive) == 0;
            if (p.op == Op::NotEquals) return line.compare(val, Qt::CaseInsensitive) != 0;
            // Contains by default.
            return line.contains(val, Qt::CaseInsensitive);
        };
        bool any = false, all = true;
        for (const QString &line : s.split(QLatin1Char('\n'))) {
            if (line.isEmpty()) continue;
            const bool m = cmpLine(line);
            any = any || m;
            all = all && m;
        }
        return (p.op == Op::NotEquals) ? all : any;
    }
    const QString s = extractText(p.field, ctx);
    switch (p.op) {
    case Op::Contains: return s.contains(val, Qt::CaseInsensitive);
    case Op::Equals:   return s.compare(val, Qt::CaseInsensitive) == 0;
    case Op::NotEquals:return s.compare(val, Qt::CaseInsensitive) != 0;
    default: return false;  // numeric ops on string field → reject
    }
}

// Hard cap on AST depth and on evaluator recursion depth. The parser and
// evaluator are both recursive (LL(1) descent + variant visit), so a
// user-typed expression like `((((((…))))))` or an attacker-controlled
// QSettings file can otherwise blow the UI thread's stack. 64 is well
// past any sensible hand-written filter and well below the safe recursion
// depth on a default 8 MiB stack.
static constexpr int kMaxExprDepth = 64;

bool evalExpr(const ExprPtr &e, const Context &ctx, int depth = 0)
{
    if (!e) return true;
    if (depth >= kMaxExprDepth) return false;
    return std::visit([&](const auto &n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, Predicate>)
            return evalPredicate(n, ctx);
        else if constexpr (std::is_same_v<T, Not>)
            return !evalExpr(n.child, ctx, depth + 1);
        else if constexpr (std::is_same_v<T, And>)
            return evalExpr(n.lhs, ctx, depth + 1) && evalExpr(n.rhs, ctx, depth + 1);
        else if constexpr (std::is_same_v<T, Or>)
            return evalExpr(n.lhs, ctx, depth + 1) || evalExpr(n.rhs, ctx, depth + 1);
        return true;
    }, e->node);
}

// ---------- Tokenizer ------------------------------------------------------

enum class TokType {
    Ident, String, Number, Op, LParen, RParen, And, Or, Not, End
};

struct Token {
    TokType type;
    QString text;
    int     pos;     // start column in input
};

class Lexer {
public:
    explicit Lexer(QString src) : m_src(std::move(src)) {}

    Token next()
    {
        skipWs();
        if (m_pos >= m_src.size()) return {TokType::End, {}, m_pos};
        const int start = m_pos;
        const QChar ch = m_src[m_pos];

        if (ch == QLatin1Char('(')) { ++m_pos; return {TokType::LParen, QStringLiteral("("), start}; }
        if (ch == QLatin1Char(')')) { ++m_pos; return {TokType::RParen, QStringLiteral(")"), start}; }

        // Operator-ish: : = != < <= > >= ~ && ||
        if (ch == QLatin1Char(':')) { ++m_pos; return {TokType::Op, QStringLiteral(":"), start}; }
        if (ch == QLatin1Char('~')) { ++m_pos; return {TokType::Op, QStringLiteral("~"), start}; }
        if (ch == QLatin1Char('=')) { ++m_pos; return {TokType::Op, QStringLiteral("="), start}; }
        if (ch == QLatin1Char('!')) {
            if (m_pos + 1 < m_src.size() && m_src[m_pos + 1] == QLatin1Char('=')) {
                m_pos += 2; return {TokType::Op, QStringLiteral("!="), start};
            }
            ++m_pos; return {TokType::Not, QStringLiteral("!"), start};
        }
        if (ch == QLatin1Char('<')) {
            if (m_pos + 1 < m_src.size() && m_src[m_pos + 1] == QLatin1Char('=')) {
                m_pos += 2; return {TokType::Op, QStringLiteral("<="), start};
            }
            ++m_pos; return {TokType::Op, QStringLiteral("<"), start};
        }
        if (ch == QLatin1Char('>')) {
            if (m_pos + 1 < m_src.size() && m_src[m_pos + 1] == QLatin1Char('=')) {
                m_pos += 2; return {TokType::Op, QStringLiteral(">="), start};
            }
            ++m_pos; return {TokType::Op, QStringLiteral(">"), start};
        }
        if (ch == QLatin1Char('&') && m_pos + 1 < m_src.size()
            && m_src[m_pos + 1] == QLatin1Char('&')) {
            m_pos += 2; return {TokType::And, QStringLiteral("&&"), start};
        }
        if (ch == QLatin1Char('|') && m_pos + 1 < m_src.size()
            && m_src[m_pos + 1] == QLatin1Char('|')) {
            m_pos += 2; return {TokType::Or, QStringLiteral("||"), start};
        }

        // Quoted string.
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            const QChar quote = ch;
            ++m_pos;
            QString s;
            while (m_pos < m_src.size() && m_src[m_pos] != quote) {
                if (m_src[m_pos] == QLatin1Char('\\') && m_pos + 1 < m_src.size()) {
                    s += m_src[m_pos + 1];
                    m_pos += 2;
                } else {
                    s += m_src[m_pos++];
                }
            }
            if (m_pos < m_src.size()) ++m_pos; // skip closing quote
            return {TokType::String, s, start};
        }

        // Bareword / identifier / number. Read until whitespace or operator-
        // significant char. The parser decides whether it's a field name,
        // a value, a boolean keyword, etc.
        QString word;
        while (m_pos < m_src.size()) {
            const QChar c = m_src[m_pos];
            if (c.isSpace()) break;
            if (QString("()<>=!~:&|").contains(c)) break;
            word += c;
            ++m_pos;
        }
        // Keywords.
        const QString lw = word.toLower();
        if (lw == QLatin1String("and")) return {TokType::And, word, start};
        if (lw == QLatin1String("or"))  return {TokType::Or,  word, start};
        if (lw == QLatin1String("not")) return {TokType::Not, word, start};
        // Identifier vs number is decided by the parser based on context.
        return {TokType::Ident, word, start};
    }

private:
    void skipWs()
    {
        while (m_pos < m_src.size() && m_src[m_pos].isSpace()) ++m_pos;
    }
    QString m_src;
    int     m_pos = 0;
};

// ---------- Parser ---------------------------------------------------------

class Parser {
public:
    explicit Parser(const QString &src) : m_lex(src), m_src(src)
    {
        m_cur = m_lex.next();
    }

    ParseResult parse()
    {
        ParseResult res;
        if (m_cur.type == TokType::End) return res;  // empty → match-all
        ExprPtr e = parseOr(0);
        if (!m_error.isEmpty()) {
            res.error    = m_error;
            res.errorPos = m_errorPos;
            return res;
        }
        if (m_cur.type != TokType::End) {
            res.error    = QStringLiteral("unexpected '%1'").arg(m_cur.text);
            res.errorPos = m_cur.pos;
            return res;
        }
        res.expr = e;
        return res;
    }

private:
    Token  m_cur;
    Lexer  m_lex;
    QString m_src;
    QString m_error;
    int     m_errorPos = -1;

    void advance() { m_cur = m_lex.next(); }
    void fail(const QString &msg, int pos)
    {
        if (m_error.isEmpty()) { m_error = msg; m_errorPos = pos; }
    }

    // Common guard: returns true if recursion would exceed kMaxExprDepth.
    // We surface a parse error (not a silent reject) so users typing too
    // many parens get a clear diagnostic.
    bool tooDeep(int depth)
    {
        if (depth < kMaxExprDepth) return false;
        fail(QStringLiteral("expression nested too deeply (max %1)")
                 .arg(kMaxExprDepth),
             m_cur.pos);
        return true;
    }

    ExprPtr parseOr(int depth)
    {
        if (tooDeep(depth)) return {};
        ExprPtr lhs = parseAnd(depth + 1);
        while (m_error.isEmpty() && m_cur.type == TokType::Or) {
            advance();
            ExprPtr rhs = parseAnd(depth + 1);
            lhs = std::make_shared<Expr>(Or{lhs, rhs});
        }
        return lhs;
    }
    ExprPtr parseAnd(int depth)
    {
        if (tooDeep(depth)) return {};
        ExprPtr lhs = parseNot(depth + 1);
        while (m_error.isEmpty() && m_cur.type == TokType::And) {
            advance();
            ExprPtr rhs = parseNot(depth + 1);
            lhs = std::make_shared<Expr>(And{lhs, rhs});
        }
        return lhs;
    }
    ExprPtr parseNot(int depth)
    {
        if (tooDeep(depth)) return {};
        if (m_cur.type == TokType::Not) {
            advance();
            ExprPtr child = parseNot(depth + 1);
            return std::make_shared<Expr>(Not{child});
        }
        return parseAtom(depth + 1);
    }
    ExprPtr parseAtom(int depth)
    {
        if (tooDeep(depth)) return {};
        if (m_cur.type == TokType::LParen) {
            advance();
            ExprPtr e = parseOr(depth + 1);
            if (m_cur.type != TokType::RParen) {
                fail(QStringLiteral("expected ')'"), m_cur.pos);
                return {};
            }
            advance();
            return e;
        }
        return parsePredicate();
    }
    ExprPtr parsePredicate()
    {
        if (m_cur.type != TokType::Ident) {
            fail(QStringLiteral("expected field name"), m_cur.pos);
            return {};
        }
        const QString fname = m_cur.text.toLower();
        const int fpos = m_cur.pos;
        auto it = fieldTable().constFind(fname);
        if (it == fieldTable().cend()) {
            fail(QStringLiteral("unknown field '%1'").arg(m_cur.text), fpos);
            return {};
        }
        advance();
        if (m_cur.type != TokType::Op) {
            fail(QStringLiteral("expected operator after field '%1'").arg(fname), m_cur.pos);
            return {};
        }
        const QString opStr = m_cur.text;
        const int opPos = m_cur.pos;
        Op op = Op::Equals;
        if      (opStr == QLatin1String(":"))  op = Op::Contains;
        else if (opStr == QLatin1String("="))  op = Op::Equals;
        else if (opStr == QLatin1String("!=")) op = Op::NotEquals;
        else if (opStr == QLatin1String("~"))  op = Op::Regex;
        else if (opStr == QLatin1String("<"))  op = Op::Lt;
        else if (opStr == QLatin1String("<=")) op = Op::Le;
        else if (opStr == QLatin1String(">"))  op = Op::Gt;
        else if (opStr == QLatin1String(">=")) op = Op::Ge;
        else { fail(QStringLiteral("unknown operator '%1'").arg(opStr), opPos); return {}; }
        advance();
        if (m_cur.type != TokType::Ident && m_cur.type != TokType::String) {
            fail(QStringLiteral("expected value after operator"), m_cur.pos);
            return {};
        }
        const QString val = m_cur.text;
        const int valPos = m_cur.pos;
        advance();

        Predicate p;
        p.field     = it->f;
        p.op        = op;
        p.textValue = val;
        if (it->isNumeric) {
            const double n = parseNumeric(val);
            if (std::isnan(n)) {
                fail(QStringLiteral("expected numeric value for field '%1'").arg(fname), valPos);
                return {};
            }
            p.numValue   = n;
            p.hasNumeric = true;
        }
        if (op == Op::Regex) {
            p.rx = QRegularExpression(val, QRegularExpression::CaseInsensitiveOption);
            if (!p.rx.isValid()) {
                fail(QStringLiteral("invalid regex: %1").arg(p.rx.errorString()), valPos);
                return {};
            }
        }
        // Numeric ordering ops require a numeric field.
        if ((op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge)
            && !it->isNumeric) {
            fail(QStringLiteral("field '%1' is not numeric").arg(fname), opPos);
            return {};
        }
        return std::make_shared<Expr>(std::move(p));
    }
};

} // namespace

// ---------- Public API -----------------------------------------------------

ParseResult parse(const QString &input)
{
    Parser p(input);
    return p.parse();
}

bool matches(const ExprPtr &expr, const Context &ctx)
{
    return evalExpr(expr, ctx);
}

QString helpHtml()
{
    return QStringLiteral(
        "<b>Filter expression syntax</b>"
        "<p>Combine predicates with <code>and</code> / <code>or</code> / <code>not</code>"
        " (or <code>&amp;&amp;</code> / <code>||</code> / <code>!</code>),"
        " group with parentheses.</p>"
        "<p><b>Predicate:</b> <code>field <i>op</i> value</code></p>"
        "<table cellpadding='2'>"
        "<tr><td><code>:</code></td><td>substring (text) / equals (numeric)</td></tr>"
        "<tr><td><code>=</code> / <code>!=</code></td><td>exact match / negation (case-insensitive)</td></tr>"
        "<tr><td><code>~</code></td><td>regex (case-insensitive)</td></tr>"
        "<tr><td><code>&lt; &lt;= &gt; &gt;=</code></td><td>numeric compare</td></tr>"
        "</table>"
        "<p><b>Text fields:</b> <code>proto</code>, <code>src</code>, <code>dst</code>,"
        " <code>host</code>, <code>iface</code>, <code>family</code>,"
        " <code>direction</code>, <code>comm</code>, <code>runtime</code>,"
        " <code>container</code>, <code>chain_has</code>, <code>reason</code></p>"
        "<p><b>Numeric fields:</b> <code>sport</code>, <code>dport</code>, <code>port</code>,"
        " <code>bytes_in</code>/<code>_out</code>/<code>bytes</code>,"
        " <code>pkts_in</code>/<code>_out</code>/<code>pkts</code>,"
        " <code>rate_in</code>/<code>_out</code>/<code>rate</code>,"
        " <code>pid</code>, <code>uid</code></p>"
        "<p><b>Attribution notes:</b> <code>container</code> matches across"
        " runtime + id + name (like <code>host</code> across endpoints);"
        " <code>chain_has</code> matches any entry in the container nesting"
        " ancestry; <code>pid=0</code> selects unattributed flows;"
        " <code>reason</code> is <code>resolved</code>/<code>forwarded</code>/"
        "<code>orphaned</code>/<code>nosocket</code> — why a flow is / isn't"
        " attributed to a local process.</p>"
        "<p><b>Byte suffixes:</b> <code>K M G T</code> (×1000),"
        " <code>Ki Mi Gi Ti</code> (×1024)</p>"
        "<p><b>Examples:</b><br>"
        "&nbsp;&nbsp;<code>proto:tcp and dport=443</code><br>"
        "&nbsp;&nbsp;<code>host:google.com</code><br>"
        "&nbsp;&nbsp;<code>src ~ ^192\\.168\\. and rate &gt; 1Mi</code><br>"
        "&nbsp;&nbsp;<code>iface=wlp228s0 and not direction:in</code><br>"
        "&nbsp;&nbsp;<code>port:80 or port:443</code><br>"
        "&nbsp;&nbsp;<code>runtime=docker and comm:nginx</code><br>"
        "&nbsp;&nbsp;<code>chain_has:kubernetes and rate &gt; 100Ki</code><br>"
        "&nbsp;&nbsp;<code>pid=0</code>  (unattributed flows only)"
        "</p>");
}

} // namespace qiftop::filter
