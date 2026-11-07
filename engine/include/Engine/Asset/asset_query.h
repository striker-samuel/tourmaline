// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

#pragma once
// THE CONTENT-AS-CODE QUERY API (project::MatchAssets - the compile-
// time asset matcher the generated project.h binds over its catalog):
// a FixedString non-type template parameter, AssetTraits<Key> (the
// key type -> catalog-kind mapping, found by ADL in the project's
// namespace), and a small CONSTEXPR REGEX ENGINE matching asset
// SYMBOLS or PATHS entirely at compile time (C++20 consteval -
// std::regex is not constexpr, and runtime matching would violate
// the content-as-code principle: the compiler turns a category
// description into a concrete list of stable ids).
//
// Regex grammar (REGEX semantics, not glob):
//   literals      world_          literal text
//   .             any single byte
//   *  +  ?       quantifiers on the preceding atom (greediness is
//                 irrelevant - the matcher computes position SETS)
//   ( ... )       grouping (full nesting, incl. alternation inside)
//   a|b           alternation
//   [abc] [a-z]   character classes (ranges); leading ^ negates
//   ^  $          anchors (start / end of the subject)
//   \x            escape a metacharacter (\. \\ \* ...); \n \t \r
// NOT supported (deliberately - asset-name matching needs none, and
// each costs complexity): backreferences, lookahead/lookbehind, lazy
// quantifiers, POSIX classes, {m,n} repetition.
//
// MATCH SEMANTICS: the pattern must match the ENTIRE subject (a full
// match - "stone" matches only the symbol "stone"). Explicit ^ / $
// anchors work as usual and are the idiomatic form; they are NOT
// implied (a bare "world_" full-matches exactly that name).
//
// THE MATCHER: the pattern compiles (constexpr) into a bounded AST;
// matching evaluates each node into a set of reachable END positions
// (a 256-bit mask) - the Thompson/Pike NFA discipline expressed over
// the AST, LINEAR in subject length with a memo cache on
// (node, start position). No backtracking paths, no exponential
// blowup; a bounded finite-state match by construction.
//
// This header is PURE C++20 (<array>/<string_view>/<cstdint> only -
// no engine types), so the GENERATED project.h includes it and tests
// can drive it with synthetic catalogs. The per-project binding
// (namespace project { template<...> consteval auto MatchAssets() }
// over project::Assets / project::AssetSymbols) is emitted by the
// generator - tools/project_pipeline/src/project_catalog.cpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace project_pipeline::query {

// ---------------------------------------------------------------------------
// FixedString: a string literal as a structural non-type parameter.
// ---------------------------------------------------------------------------
template <std::size_t N> struct FixedString {
    char chars[N] = {}; // N includes the trailing NUL

    constexpr FixedString(const char (&lit)[N]) {
        for (std::size_t i = 0; i < N; ++i)
            chars[i] = lit[i];
    }
    constexpr std::string_view view() const {
        std::size_t len = 0;
        while (len + 1 < N && chars[len] != '\0')
            ++len;
        return std::string_view(chars, len);
    }
};
template <std::size_t N> FixedString(const char (&)[N]) -> FixedString<N>;

// What a query matches: the generated SYMBOL name (the C++ identifier
// the catalog emits, e.g. "stone" - the default) or the asset's
// content PATH (e.g. "content/models/game/shared/stone.png").
enum class MatchField : std::uint8_t {
    Symbol,
    Path,
};

// THE TRAIT PRIMARY: key type -> catalog-kind predicate. The
// generated project.h SPECIALIZES this for its own Key types (the
// qualified-id form, at the header's outer scope - the standard's
// enclosing-namespace rule). The specializations are the ONLY
// per-project part of the trait; the query core consults them at
// instantiation.
template <typename Key> struct AssetTraits; // specialized by the generated project.h

// ---------------------------------------------------------------------------
// Pattern diagnostics (a malformed pattern is a COMPILE-TIME error in
// MatchAssets; CompilePattern is also usable at runtime for tests).
// ---------------------------------------------------------------------------
enum class PatternError : std::uint8_t {
    None = 0,
    EmptyGroup,       // "()"
    UnclosedGroup,    // "(abc"
    UnmatchedClose,   // "abc)"
    EmptyAlternation, // "|a" / "a|" / "(|a)"
    QuantifierNoAtom, // "*a" / "(+)" / "a**"
    BadEscape,        // trailing "\" or "\q"
    UnclosedClass,    // "[a-z" (a "[]" is unclosed too: the POSIX
                      // first-']'-is-literal rule makes an empty
                      // class unrepresentable)
    BadClassRange,    // "[z-a]"
    TooComplex,       // node/depth/subject budget exceeded
};

constexpr const char* PatternErrorText(PatternError e) {
    switch (e) {
    case PatternError::None:
        return "ok";
    case PatternError::EmptyGroup:
        return "empty group '()'";
    case PatternError::UnclosedGroup:
        return "unclosed '('";
    case PatternError::UnmatchedClose:
        return "unmatched ')'";
    case PatternError::EmptyAlternation:
        return "empty alternation side";
    case PatternError::QuantifierNoAtom:
        return "quantifier with nothing to repeat";
    case PatternError::BadEscape:
        return "bad escape";
    case PatternError::UnclosedClass:
        return "unclosed '['";
    case PatternError::BadClassRange:
        return "class range end < start";
    case PatternError::TooComplex:
        return "pattern too complex";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The bounded AST + the compiled pattern.
// ---------------------------------------------------------------------------
constexpr std::size_t kMaxPatternNodes = 96;
constexpr std::size_t kMaxSubjectLen = 256;

enum class NodeKind : std::uint8_t {
    Char,  // one literal byte
    Any,   // one arbitrary byte
    Class, // one byte in the set
    Bol,   // ^ (zero width: position 0)
    Eol,   // $ (zero width: position len)
    Seq,   // a then b
    Alt,   // a or b
    Star,  // a* (zero or more)
    Plus,  // a+ (one or more)
    Opt,   // a? (optional)
};

struct Node {
    NodeKind kind = NodeKind::Char;
    unsigned char byte = 0;        // Char
    std::uint8_t setBits[32] = {}; // Class (a 256-bit byte set)
    std::int16_t a = -1;           // first child / left
    std::int16_t b = -1;           // right (Seq/Alt)
};

struct CompiledPattern {
    Node nodes[kMaxPatternNodes] = {};
    std::int16_t root = -1;
    std::int16_t count = 0;
    PatternError err = PatternError::None;

    constexpr bool ok() const {
        return err == PatternError::None;
    }
};

// ---------------------------------------------------------------------------
// CompilePattern: the recursive-descent parser.
//   alternation := concatenation ('|' concatenation)*
//   concatenation := piece*
//   piece        := atom ('*' | '+' | '?')?
//   atom         := literal | '.' | '^' | '$' | class | group | escape
// (constexpr throughout; usable in consteval AND at runtime.)
// ---------------------------------------------------------------------------
namespace detail {

constexpr void bitSet(std::uint8_t (&bits)[32], unsigned b) {
    bits[b >> 3] |= std::uint8_t(1u << (b & 7));
}
constexpr bool bitTest(const std::uint8_t (&bits)[32], unsigned b) {
    return (bits[b >> 3] & (1u << (b & 7))) != 0;
}

struct Parser {
    std::string_view pat;
    std::size_t pos = 0;
    CompiledPattern out;
    std::size_t depth = 0;

    constexpr bool atEnd() const {
        return pos >= pat.size();
    }
    constexpr char peek() const {
        return atEnd() ? '\0' : pat[pos];
    }
    constexpr void adv() {
        if (!atEnd())
            ++pos;
    }

    constexpr std::int16_t newNode(Node n) {
        if (out.count >= std::int16_t(kMaxPatternNodes)) {
            out.err = PatternError::TooComplex;
            return -1;
        }
        out.nodes[out.count] = n;
        return out.count++;
    }

    // ---- atom (unquantified) ----
    constexpr std::int16_t parseAtom() {
        const char c = peek();
        if (c == '(') {
            adv();
            if (peek() == ')') {
                out.err = PatternError::EmptyGroup;
                return -1;
            }
            ++depth;
            const std::int16_t inner = parseAlternation();
            --depth;
            if (out.err != PatternError::None)
                return -1;
            if (peek() != ')') {
                out.err = PatternError::UnclosedGroup;
                return -1;
            }
            adv();
            return inner;
        }
        if (c == '[') {
            adv();
            Node cls;
            cls.kind = NodeKind::Class;
            bool negated = false;
            if (peek() == '^') {
                negated = true;
                adv();
            }
            // ']' as the first class member is a literal (POSIX) -
            // every class therefore has >= 1 member.
            bool first = true;
            while (!atEnd() && (first || peek() != ']')) {
                first = false;
                unsigned char lo = static_cast<unsigned char>(peek());
                if (lo == '\\') {
                    adv();
                    if (atEnd()) {
                        out.err = PatternError::BadEscape;
                        return -1;
                    }
                    const char e = peek();
                    lo = e == 'n'   ? '\n'
                         : e == 't' ? '\t'
                         : e == 'r' ? '\r'
                                    : static_cast<unsigned char>(e);
                }
                adv();
                unsigned char hi = lo;
                if (peek() == '-' && pos + 1 < pat.size() && pat[pos + 1] != ']') {
                    adv(); // '-'
                    hi = static_cast<unsigned char>(peek());
                    if (hi == '\\') {
                        adv();
                        if (atEnd()) {
                            out.err = PatternError::BadEscape;
                            return -1;
                        }
                        hi = static_cast<unsigned char>(peek());
                    }
                    adv();
                    if (hi < lo) {
                        out.err = PatternError::BadClassRange;
                        return -1;
                    }
                }
                for (unsigned b = lo; b <= hi; ++b)
                    bitSet(cls.setBits, b);
            }
            if (atEnd()) {
                out.err = PatternError::UnclosedClass;
                return -1;
            }
            adv(); // ']'
            // (An "empty class" is unrepresentable: the POSIX
            // first-']'-is-literal rule means a '[' always consumes a
            // member before it can close - "[]" is simply unclosed.)
            if (negated) {
                // Negation REPLACES the set with its complement (the
                // inverted bits go into a FRESH array - OR-ing into
                // the existing set would yield "all 256 bytes").
                std::uint8_t inverted[32] = {};
                for (unsigned b = 0; b < 256; ++b)
                    if (!bitTest(cls.setBits, b))
                        bitSet(inverted, b);
                for (std::size_t w = 0; w < 32; ++w)
                    cls.setBits[w] = inverted[w];
            }
            return newNode(cls);
        }
        if (c == '.') {
            adv();
            return newNode(Node{NodeKind::Any, 0, {}, -1, -1});
        }
        if (c == '^') {
            adv();
            return newNode(Node{NodeKind::Bol, 0, {}, -1, -1});
        }
        if (c == '$') {
            adv();
            return newNode(Node{NodeKind::Eol, 0, {}, -1, -1});
        }
        if (c == '\\') {
            adv();
            if (atEnd()) {
                out.err = PatternError::BadEscape;
                return -1;
            }
            const char e = peek();
            const unsigned char b = e == 'n'   ? '\n'
                                    : e == 't' ? '\t'
                                    : e == 'r' ? '\r'
                                               : static_cast<unsigned char>(e);
            adv();
            return newNode(Node{NodeKind::Char, b, {}, -1, -1});
        }
        // A quantifier / alternation char here means "no atom".
        if (c == '*' || c == '+' || c == '?') {
            out.err = PatternError::QuantifierNoAtom;
            return -1;
        }
        if (c == '|' || c == ')') {
            // Not an atom: the caller (concatenation) stops on these.
            return -1;
        }
        adv();
        return newNode(Node{NodeKind::Char, static_cast<unsigned char>(c), {}, -1, -1});
    }

    // ---- concatenation: pieces until '|' / ')' / end ----
    constexpr std::int16_t parseConcat(bool& emptyRun) {
        emptyRun = true;
        std::int16_t run = -1;
        while (!atEnd() && peek() != '|' && peek() != ')') {
            std::int16_t atom = parseAtom();
            if (out.err != PatternError::None)
                return -1;
            if (atom < 0)
                break; // e.g. a trailing '\' consumed weirdly - bail ok
            // The quantifier.
            if (peek() == '*' || peek() == '+' || peek() == '?') {
                const char q = peek();
                adv();
                if (peek() == '*' || peek() == '+' || peek() == '?') {
                    out.err = PatternError::QuantifierNoAtom; // "a**"
                    return -1;
                }
                Node n;
                n.kind = q == '*' ? NodeKind::Star : q == '+' ? NodeKind::Plus : NodeKind::Opt;
                n.a = atom;
                atom = newNode(n);
                if (out.err != PatternError::None)
                    return -1;
            }
            if (run < 0) {
                run = atom;
                emptyRun = false;
            } else {
                Node seq;
                seq.kind = NodeKind::Seq;
                seq.a = run;
                seq.b = atom;
                run = newNode(seq);
                if (out.err != PatternError::None)
                    return -1;
            }
        }
        return run;
    }

    // ---- alternation ----
    constexpr std::int16_t parseAlternation() {
        bool empty = true;
        std::int16_t alt = parseConcat(empty);
        if (out.err != PatternError::None)
            return -1;
        if (empty) {
            out.err = PatternError::EmptyAlternation;
            return -1;
        }
        while (peek() == '|') {
            adv();
            bool emptyRight = true;
            const std::int16_t right = parseConcat(emptyRight);
            if (out.err != PatternError::None)
                return -1;
            if (emptyRight) {
                out.err = PatternError::EmptyAlternation;
                return -1;
            }
            Node n;
            n.kind = NodeKind::Alt;
            n.a = alt;
            n.b = right;
            alt = newNode(n);
            if (out.err != PatternError::None)
                return -1;
        }
        return alt;
    }
};

// The matcher: node -> the SET of end positions reachable from a
// start position, as a bitset over [0, kMaxSubjectLen] (bit p = "a
// match can end at p", exclusive). Zero-width nodes contribute their
// own position; Seq unions the right side over every left end; the
// closure quantifiers iterate to a fixpoint (bounded by subject
// length). A MEMO cache on (node, start) keeps the evaluation linear.
struct SubjectSet {
    std::uint8_t bits[(kMaxSubjectLen + 8) / 8] = {};
    constexpr void add(std::size_t p) {
        if (p <= kMaxSubjectLen)
            bits[p >> 3] |= std::uint8_t(1u << (p & 7));
    }
    constexpr bool has(std::size_t p) const {
        return p <= kMaxSubjectLen && (bits[p >> 3] & (1u << (p & 7))) != 0;
    }
};

struct MemoEntry {
    std::int16_t node = -1;
    std::size_t start = 0;
    SubjectSet set{};
    bool valid = false;
};
constexpr std::size_t kMemoEntries = 512;

struct Matcher {
    const CompiledPattern& cp;
    std::string_view subject;
    MemoEntry memo[kMemoEntries];

    // (The memo is a direct-mapped cache keyed by (node, start)
    // hashed - collisions just recompute, never wrong.)
    constexpr std::size_t memoSlot(std::int16_t node, std::size_t start) const {
        return (std::size_t(node) * 131u + start * 17u) % kMemoEntries;
    }

    // The node evaluation.
    constexpr SubjectSet eval(std::int16_t nodeIdx, std::size_t start) {
        SubjectSet out;
        if (nodeIdx < 0 || nodeIdx >= cp.count)
            return out;
        if (start > subject.size() || subject.size() > kMaxSubjectLen)
            return out;
        const std::size_t slot = memoSlot(nodeIdx, start);
        if (memo[slot].valid && memo[slot].node == nodeIdx && memo[slot].start == start)
            return memo[slot].set;
        const Node& n = cp.nodes[nodeIdx];
        switch (n.kind) {
        case NodeKind::Char:
            if (start < subject.size() && static_cast<unsigned char>(subject[start]) == n.byte)
                out.add(start + 1);
            break;
        case NodeKind::Any:
            if (start < subject.size())
                out.add(start + 1);
            break;
        case NodeKind::Class:
            if (start < subject.size() &&
                bitTest(n.setBits, static_cast<unsigned char>(subject[start])))
                out.add(start + 1);
            break;
        case NodeKind::Bol:
            if (start == 0)
                out.add(start);
            break;
        case NodeKind::Eol:
            if (start == subject.size())
                out.add(start);
            break;
        case NodeKind::Seq: {
            const SubjectSet leftEnds = eval(n.a, start);
            for (std::size_t p = 0; p <= subject.size(); ++p)
                if (leftEnds.has(p)) {
                    const SubjectSet rightEnds = eval(n.b, p);
                    for (std::size_t q = 0; q <= subject.size(); ++q)
                        if (rightEnds.has(q))
                            out.add(q);
                }
            break;
        }
        case NodeKind::Alt: {
            const SubjectSet a = eval(n.a, start);
            const SubjectSet b = eval(n.b, start);
            for (std::size_t p = 0; p <= subject.size(); ++p)
                if (a.has(p) || b.has(p))
                    out.add(p);
            break;
        }
        case NodeKind::Opt: {
            out.add(start); // the empty route
            const SubjectSet inner = eval(n.a, start);
            for (std::size_t p = 0; p <= subject.size(); ++p)
                if (inner.has(p))
                    out.add(p);
            break;
        }
        case NodeKind::Star: {
            // The closure fixpoint: reach = {start}; repeatedly run
            // the child from every reached position, adding new ends.
            SubjectSet reach;
            reach.add(start);
            bool grew = true;
            while (grew) {
                grew = false;
                for (std::size_t p = 0; p <= subject.size(); ++p) {
                    if (!reach.has(p))
                        continue;
                    const SubjectSet ends = eval(n.a, p);
                    for (std::size_t q = 0; q <= subject.size(); ++q) {
                        if (ends.has(q) && !reach.has(q)) {
                            reach.add(q);
                            grew = true;
                        }
                    }
                }
            }
            out = reach;
            break;
        }
        case NodeKind::Plus: {
            // one child match, then the star closure over its ends
            const SubjectSet first = eval(n.a, start);
            SubjectSet reach;
            for (std::size_t p = 0; p <= subject.size(); ++p)
                if (first.has(p))
                    reach.add(p);
            bool grew = true;
            while (grew) {
                grew = false;
                for (std::size_t p = 0; p <= subject.size(); ++p) {
                    if (!reach.has(p))
                        continue;
                    const SubjectSet ends = eval(n.a, p);
                    for (std::size_t q = 0; q <= subject.size(); ++q) {
                        if (ends.has(q) && !reach.has(q)) {
                            reach.add(q);
                            grew = true;
                        }
                    }
                }
            }
            out = reach;
            break;
        }
        }
        memo[slot].node = nodeIdx;
        memo[slot].start = start;
        memo[slot].set = out;
        memo[slot].valid = true;
        return out;
    }
};

} // namespace detail

// Compile a pattern (constexpr; also usable at runtime for tests).
constexpr CompiledPattern CompilePattern(std::string_view pattern) {
    detail::Parser p{pattern};
    if (pattern.empty())
        return p.out; // an empty pattern matches nothing (err None)
    const std::int16_t root = p.parseAlternation();
    if (p.out.err != PatternError::None)
        return p.out;
    if (!p.atEnd()) {
        // parseAlternation only stops early on an unmatched ')'.
        p.out.err = PatternError::UnmatchedClose;
        return p.out;
    }
    p.out.root = root;
    return p.out;
}

// Full-match a compiled pattern against a subject.
constexpr bool MatchSubject(const CompiledPattern& cp, std::string_view subject) {
    if (!cp.ok() || cp.root < 0)
        return false;
    if (subject.size() > kMaxSubjectLen)
        return false;
    detail::Matcher m{cp, subject, {}};
    const detail::SubjectSet ends = m.eval(cp.root, 0);
    return ends.has(subject.size());
}

// Convenience: compile + match in one (consteval-friendly). An EMPTY
// pattern compiles ok but matches NOTHING (a deliberate query
// semantic - "no description" is not "everything"; use ".*").
constexpr bool MatchPattern(std::string_view pattern, std::string_view subject) {
    const CompiledPattern cp = CompilePattern(pattern);
    return MatchSubject(cp, subject);
}

// ---------------------------------------------------------------------------
// THE QUERY CORE: filter a catalog into a precisely sized key array.
//
// The generated project.h provides a CATALOG CARRIER type - static
// constexpr accessors, no object NTTPs (MSVC rejects
// string_view-bearing aggregates as non-type template parameters; a
// carrier type sidesteps the whole structural-NTTP question):
//
//   struct AssetCatalog {
//       static constexpr auto Rows() { return Assets; }
//       static constexpr auto Symbols() { return AssetSymbols; }
//   };
//   template<typename Key, FixedString Pattern, MatchField Field>
//   consteval auto MatchAssets() {
//       return project_pipeline::query::MatchAssetsImpl<
//           Key, Pattern, Field, AssetCatalog>();
//   }
//
// `AssetTraits<Key>` is looked up unqualified at instantiation - the
// project-namespace specializations the generated header emits (the
// primary is declared above). The trait contract:
// `static constexpr bool Matches(<row kind type>)`.
//
// Row contract: `row.id` (Key-constructible), `row.kind`,
// `row.path` (string_view-constructible). Symbols: rows' order.
// Determinism: results follow ROW order (the catalog's emission
// order - bucket order, id-sorted within).
// ---------------------------------------------------------------------------

template <typename Key, FixedString Pattern, MatchField Field, typename Catalog>
consteval auto MatchAssetsImpl() {
    constexpr auto rows = Catalog::Rows();
    constexpr auto symbols = Catalog::Symbols();
    constexpr CompiledPattern cp = CompilePattern(Pattern.view());
    static_assert(cp.ok(), "MatchAssets: invalid pattern (see PatternErrorText)");

    // Pass 1: count (an IIFE keeps the loop out of the initializer).
    constexpr std::size_t count = [&] {
        std::size_t n = 0;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!AssetTraits<Key>::Matches(rows[i].kind))
                continue;
            const std::string_view subject = Field == MatchField::Symbol
                                                 ? std::string_view(symbols[i])
                                                 : std::string_view(rows[i].path);
            if (MatchSubject(cp, subject))
                ++n;
        }
        return n;
    }();

    // Pass 2: fill the exact-size array (catalog order).
    std::array<Key, count> out{};
    std::size_t i = 0;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        if (!AssetTraits<Key>::Matches(rows[r].kind))
            continue;
        const std::string_view subject = Field == MatchField::Symbol
                                             ? std::string_view(symbols[r])
                                             : std::string_view(rows[r].path);
        if (MatchSubject(cp, subject)) {
            out[i] = Key{rows[r].id};
            ++i;
        }
    }
    return out;
}

} // namespace project_pipeline::query
