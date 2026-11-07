// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

// asset_query_tests: the compile-time asset-query engine (the
// MatchAssets core - Engine/Asset/asset_query.h) over a SYNTHETIC
// catalog shaped exactly like a generated project.h (the same Key /
// AssetDesc / AssetSymbols / AssetTraits forms the emitter emits).
// The regex engine's CompilePattern/MatchSubject are constexpr AND
// runtime-usable, so the invalid-pattern matrix runs as ordinary
// checks; the query itself is pinned by static_asserts (compile-time
// proof: matching, kind filtering, empty results, exact sizing,
// deterministic ordering).
//
// Hand-rolled runner (the test_* conventions), CPU-only.

#include <Engine/Asset/asset_query.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
    ++g_checks;
    if (cond) {
        std::cout << "PASS " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "FAIL " << name << "\n";
    }
}

} // namespace
  // MSVC rejects a cross-namespace specialization inside an anonymous
  // one - and the generated header emits them at outer scope anyway.)

// ---------------------------------------------------------------------------
// A synthetic generated catalog (the emitter's exact shapes).
// ---------------------------------------------------------------------------
namespace project {

using Id = std::uint64_t;

struct SceneKey {
    Id id = 0;
    constexpr operator Id() const {
        return id;
    }
};
struct TextureKey {
    Id id = 0;
    constexpr operator Id() const {
        return id;
    }
};
struct MeshKey {
    Id id = 0;
    constexpr operator Id() const {
        return id;
    }
};
struct SoundKey {
    Id id = 0;
    constexpr operator Id() const {
        return id;
    }
};
struct ShaderKey {
    Id id = 0;
    constexpr operator Id() const {
        return id;
    }
};
struct GiKey {
    Id id = 0;
    constexpr operator Id() const {
        return id;
    }
};

enum class AssetKind : std::uint8_t {
    Nil = 0,
    Scene = 1,
    Texture = 2,
    Mesh = 3,
    Sound = 4,
    Shader = 5,
    Gi = 6,
};

struct AssetDesc {
    Id id = 0;
    AssetKind kind = AssetKind::Nil;
    std::uint32_t pakIndex;
    std::string_view path;
    std::string_view sceneName;
};

inline constexpr std::array Assets = {
    AssetDesc{SceneKey{0x1000}, AssetKind::Scene, 1, "content/scenes/game.tour", ""},
    AssetDesc{TextureKey{0x2000},
              AssetKind::Texture,
              0,
              "content/models/game/shared/stone.png",
              ""},
    AssetDesc{TextureKey{0x2001}, AssetKind::Texture, 0, "content/ui/ui_panel.png", ""},
    AssetDesc{TextureKey{0x2002}, AssetKind::Texture, 0, "content/ui/ui_icon_0.png", ""},
    AssetDesc{TextureKey{0x2003}, AssetKind::Texture, 0, "content/ui/ui_icon_1.png", ""},
    AssetDesc{MeshKey{0x3000}, AssetKind::Mesh, 1, "content/models/game/world_tree.geometry", ""},
    AssetDesc{MeshKey{0x3001}, AssetKind::Mesh, 1, "content/models/game/world_rock.geometry", ""},
    AssetDesc{MeshKey{0x3002}, AssetKind::Mesh, 1, "content/models/game/prop_barrel.geometry", ""},
    AssetDesc{SoundKey{0x4000}, AssetKind::Sound, 0, "content/audio/ambient_wind.wav", ""},
    AssetDesc{SoundKey{0x4001}, AssetKind::Sound, 0, "content/audio/ambient_rain.wav", ""},
    AssetDesc{SoundKey{0x4002}, AssetKind::Sound, 0, "content/audio/bells.wav", ""},
    AssetDesc{ShaderKey{0x5000},
              AssetKind::Shader,
              0,
              "content/shaders/project_lit.frag.hlsl.spv",
              ""},
    AssetDesc{GiKey{0x6000}, AssetKind::Gi, 1, "content/gi/interior_house.givolumemanifest", ""},
    AssetDesc{GiKey{0x6001}, AssetKind::Gi, 1, "content/gi/night_city.givolumemanifest", ""},
};

// The symbol table (compile-time query metadata; NOT in AssetDesc -
// the runtime catalog ABI is untouched).
inline constexpr std::array AssetSymbols = {
    std::string_view("game"),
    std::string_view("stone"),
    std::string_view("ui_panel"),
    std::string_view("ui_icon_0"),
    std::string_view("ui_icon_1"),
    std::string_view("world_tree"),
    std::string_view("world_rock"),
    std::string_view("prop_barrel"),
    std::string_view("ambient_wind"),
    std::string_view("ambient_rain"),
    std::string_view("bells"),
    std::string_view("project_lit"),
    std::string_view("interior_house"),
    std::string_view("night_city"),
};

} // namespace project

// The trait specializations (the qualified-id form at this enclosing
// scope - exactly what the generated header emits).
template <> struct project_pipeline::query::AssetTraits<project::SceneKey> {
    static constexpr bool Matches(project::AssetKind k) {
        return k == project::AssetKind::Scene;
    }
};
template <> struct project_pipeline::query::AssetTraits<project::TextureKey> {
    static constexpr bool Matches(project::AssetKind k) {
        return k == project::AssetKind::Texture;
    }
};
template <> struct project_pipeline::query::AssetTraits<project::MeshKey> {
    static constexpr bool Matches(project::AssetKind k) {
        return k == project::AssetKind::Mesh;
    }
};
template <> struct project_pipeline::query::AssetTraits<project::SoundKey> {
    static constexpr bool Matches(project::AssetKind k) {
        return k == project::AssetKind::Sound;
    }
};
template <> struct project_pipeline::query::AssetTraits<project::ShaderKey> {
    static constexpr bool Matches(project::AssetKind k) {
        return k == project::AssetKind::Shader;
    }
};
template <> struct project_pipeline::query::AssetTraits<project::GiKey> {
    static constexpr bool Matches(project::AssetKind k) {
        return k == project::AssetKind::Gi;
    }
};

namespace project {

// The catalog carrier (the query core's data seam - the exact shape
// the generator emits; no object NTTPs, just constexpr accessors).
struct AssetCatalog {
    static constexpr auto Rows() {
        return Assets;
    }
    static constexpr auto Symbols() {
        return AssetSymbols;
    }
};

// THE BINDING (the exact shape the generator emits).
template <
    typename Key,
    ::project_pipeline::query::FixedString Pattern,
    ::project_pipeline::query::MatchField Field = ::project_pipeline::query::MatchField::Symbol>
consteval auto MatchAssets() {
    return ::project_pipeline::query::MatchAssetsImpl<Key, Pattern, Field, AssetCatalog>();
}

} // namespace project

// ---------------------------------------------------------------------------
// Compile-time pins (static_assert - the API's whole point).
// ---------------------------------------------------------------------------
namespace pq = project_pipeline::query;

static_assert(project::AssetKind{} == project::AssetKind::Nil);
static_assert(project::AssetKind{} != project::AssetKind::Scene);

// Exact-size typed results.
static_assert(project::MatchAssets<project::MeshKey, "^world_.*$">().size() == 2);
static_assert(project::MatchAssets<project::TextureKey, "^ui_.*$">().size() == 3);
static_assert(project::MatchAssets<project::SoundKey, "^ambient_.*$">().size() == 2);
static_assert(project::MatchAssets<project::GiKey, "^interior_.*$">().size() == 1);
// Unanchored full-match semantics: the pattern matches the WHOLE name.
static_assert(project::MatchAssets<project::SoundKey, "bells">().size() == 1);
static_assert(project::MatchAssets<project::SoundKey, "bell">().size() == 0);
// Zero matches: an EMPTY array (still iterable, size 0 - the exact
// shape pointer/count batch APIs want).
static_assert(project::MatchAssets<project::MeshKey, "^nope_.*$">().size() == 0);
static_assert(project::MatchAssets<project::MeshKey, "^nope_.*$">().begin() ==
              project::MatchAssets<project::MeshKey, "^nope_.*$">().end());
// Kind filtering: the same pattern over a different kind.
static_assert(project::MatchAssets<project::ShaderKey, "^project_.*$">().size() == 1);
static_assert(project::MatchAssets<project::SceneKey, "^.*$">().size() == 1);
// Deterministic ordering (catalog order, id ascending within kind).
static_assert(project::MatchAssets<project::TextureKey, "^ui_.*$">()[0].id == 0x2001);
static_assert(project::MatchAssets<project::TextureKey, "^ui_.*$">()[2].id == 0x2003);
static_assert(project::MatchAssets<project::MeshKey, "^world_.*$">()[0].id == 0x3000);
// Alternation + groups.
static_assert(project::MatchAssets<project::MeshKey, "^world_(tree|rock)$">().size() == 2);
static_assert(project::MatchAssets<project::MeshKey, "^(world_|prop_).*$">().size() == 3);
static_assert(project::MatchAssets<project::GiKey, "^(day|night)_.*$">().size() == 1);
// Classes + ranges + negation + quantifiers.
static_assert(project::MatchAssets<project::TextureKey, "^ui_icon_[0-9]+$">().size() == 2);
static_assert(project::MatchAssets<project::TextureKey,
                                   "^ui_[a-z_]+$">()
                  .size() == 1); // "ui_panel" (icons carry digits)
static_assert(project::MatchAssets<project::TextureKey, "^ui_[a-z_0-9]+$">().size() == 3);
static_assert(project::MatchAssets<project::TextureKey, "^ui_[^0-9]+$">().size() == 1);
static_assert(project::MatchAssets<project::TextureKey, "^ui_(panel|icon_([01]))$">().size() == 3);
static_assert(project::MatchAssets<project::MeshKey, "^world_(tree|rock)+$">().size() == 2);
static_assert(project::MatchAssets<project::MeshKey, "^world_treeo?$">().size() == 1);
// Escapes: the path field with a LITERAL period.
static_assert(project::MatchAssets<project::TextureKey,
                                   R"(^content/models/game/shared/stone\.png$)",
                                   pq::MatchField::Path>()
                  .size() == 1);
static_assert(project::MatchAssets<project::TextureKey, "^.*stone\\.png$", pq::MatchField::Path>()
                  .size() == 1);
// Path-field prefix matching.
static_assert(project::MatchAssets<project::TextureKey, "^content/ui/.*", pq::MatchField::Path>()
                  .size() == 3);
static_assert(project::MatchAssets<project::GiKey,
                                   "^content/gi/.*\\.givolumemanifest$",
                                   pq::MatchField::Path>()
                  .size() == 2);
// The Any atom, quantified: "stone" and "bells" are both 5 symbols.
static_assert(project::MatchAssets<project::SoundKey, "^.....$">().size() ==
              1); // "bells" (the only 5-char SOUND symbol)
static_assert(project::MatchAssets<project::TextureKey, "^.....$">().size() == 1); // "stone"
// Mixed: symbol ".*_0" over textures (icons).
static_assert(project::MatchAssets<project::TextureKey, ".*_0">().size() == 1);

// ---------------------------------------------------------------------------
// Runtime checks: the pattern compiler's diagnostics matrix + engine
// behaviors the tests can exercise without a compiler at hand.
// ---------------------------------------------------------------------------
namespace {

void TestPatternDiagnostics() {
    using pq::CompilePattern;
    using pq::PatternError;
    auto errOf = [](const char* p) { return CompilePattern(p).err; };
    Check(errOf("^world_.*$") == PatternError::None, "qrx.ok-plain");
    Check(errOf("^(a|b)*c?[0-9]+\\.$") == PatternError::None, "qrx.ok-full-grammar");
    Check(errOf("") == PatternError::None, "qrx.ok-empty");
    Check(errOf("()") == PatternError::EmptyGroup, "qrx.bad-empty-group");
    Check(errOf("(abc") == PatternError::UnclosedGroup, "qrx.bad-unclosed-group");
    Check(errOf("abc)") == PatternError::UnmatchedClose, "qrx.bad-unmatched-close");
    Check(errOf("|abc") == PatternError::EmptyAlternation, "qrx.bad-leading-bar");
    Check(errOf("abc|") == PatternError::EmptyAlternation, "qrx.bad-trailing-bar");
    Check(errOf("a||b") == PatternError::EmptyAlternation, "qrx.bad-empty-branch");
    Check(errOf("*abc") == PatternError::QuantifierNoAtom, "qrx.bad-leading-star");
    Check(errOf("a**") == PatternError::QuantifierNoAtom, "qrx.bad-double-star");
    Check(errOf("a?+") == PatternError::QuantifierNoAtom, "qrx.bad-mixed-quantifier");
    Check(errOf("abc\\") == PatternError::BadEscape, "qrx.bad-trailing-backslash");
    Check(errOf("[a-z") == PatternError::UnclosedClass, "qrx.bad-unclosed-class");
    Check(errOf("[]") == PatternError::UnclosedClass, "qrx.bad-empty-class-is-unclosed");
    Check(errOf("[z-a]") == PatternError::BadClassRange, "qrx.bad-class-range");
    Check(CompilePattern("abc").ok(), "qrx.compile-ok");
    Check(!CompilePattern("(abc").ok(), "qrx.compile-fail-flag");
}

void TestEngineBehavior() {
    using pq::MatchPattern;
    Check(MatchPattern("^world_.*$", "world_tree"), "qrx.match-prefix-star");
    Check(!MatchPattern("^world_.*$", "prop_barrel"), "qrx.match-reject");
    Check(MatchPattern("bells", "bells"), "qrx.full-match-exact");
    Check(!MatchPattern("bell", "bells"), "qrx.full-match-rejects-partial");
    Check(MatchPattern("^bells$", "bells"), "qrx.anchors-exact");
    Check(!MatchPattern("^bells$", "xbells"), "qrx.anchor-bol");
    Check(!MatchPattern("^bells$", "bellsx"), "qrx.anchor-eol");
    Check(MatchPattern(".*", "anything at all"), "qrx.dot-star");
    Check(MatchPattern("a?b", "b"), "qrx.opt-empty");
    Check(MatchPattern("a?b", "ab"), "qrx.opt-filled");
    Check(MatchPattern("(ab)+c", "ababc"), "qrx.plus-group");
    Check(!MatchPattern("(ab)+c", "abac"), "qrx.plus-group-reject");
    Check(MatchPattern("a|b|c", "b"), "qrx.alt-middle");
    Check(MatchPattern("[0-9]+", "12345"), "qrx.class-digits");
    Check(!MatchPattern("[0-9]+", "12a45"), "qrx.class-digits-reject");
    Check(MatchPattern("[^0-9]+", "abc"), "qrx.class-negated");
    Check(!MatchPattern("[^0-9]+", "ab1"), "qrx.class-negated-reject");
    Check(MatchPattern("a\\.b", "a.b"), "qrx.escape-dot");
    Check(!MatchPattern("a\\.b", "axb"), "qrx.escape-dot-reject");
    Check(MatchPattern("\\n", "\n"), "qrx.escape-newline");
    Check(MatchPattern("[]]", "]"), "qrx.class-literal-close-first");
    // The closure fixpoint (Star over a multi-char child).
    Check(MatchPattern("(ab)*", "ababab"), "qrx.star-group-repeat");
    Check(MatchPattern("(ab)*", ""), "qrx.star-group-empty");
    Check(!MatchPattern("(ab)*", "aba"), "qrx.star-group-partial");
    // The pathological backtracking shape stays BOUNDED here (the
    // matcher computes position sets; this is a smoke check, not a
    // complexity proof).
    Check(!MatchPattern(".*.*.*.*.*z", "aaaaaaaaaaaaaaaaaaaaaaaa"), "qrx.no-backtracking-blowup");
    // Empty pattern: compiles ok, matches nothing (the deliberate
    // query semantic - "no description" is not "everything"; ".*" is).
    Check(!MatchPattern("", "anything"), "qrx.empty-matches-nothing");
    Check(!MatchPattern("", ""), "qrx.empty-matches-nothing-even-empty");
}

void TestGeneratedShapes() {
    // The binding's outputs usable in the pointer/count batch shape
    // (the intended application experience).
    constexpr auto worldMeshes = project::MatchAssets<project::MeshKey, "^world_.*$">();
    static_assert(worldMeshes.size() == 2);
    Check(worldMeshes.size() == 2 && worldMeshes[0].id == 0x3000 && worldMeshes[1].id == 0x3001,
          "qrx.batch-shape-deterministic");
    constexpr auto none = project::MatchAssets<project::MeshKey, "^nope_">();
    Check(none.size() == 0 && none.begin() == none.end(), "qrx.empty-array-iterable");
}

} // namespace

int main() {
    TestPatternDiagnostics();
    TestEngineBehavior();
    TestGeneratedShapes();
    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << " (" << g_checks << " checks)\n";
    return g_failures == 0 ? 0 : 1;
}
