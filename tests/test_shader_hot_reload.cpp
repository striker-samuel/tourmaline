// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// shader_hot_reload_tests: CPU-only regression tests for the watched
// shader hot-reload service (Engine/Utils/shader_hot_reload.h): the
// in-process DXC recompile (include handling + the variant twins),
// SPIR-V validation, the byte-identical no-op, the LAST-KNOWN-GOOD
// contract (a broken source never touches the previous .spv), the
// .hlsli coarse invalidation, and the watcher-driven flow. No GPU:
// the service only lands BYTES (the shader library reads them).

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <Engine/Utils/shader_hot_reload.h>
#include <Engine/Utils/shader_library.h>
// The consumer leg's coverage set (LiveSwapCovers — a static
// classifier; including the renderer header pulls no GPU state).
#include <Engine/Renderer/WorldRenderer.h>

namespace {

namespace fs = std::filesystem;

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

const char* kVert = R"(// the tiny reload-test vertex stage
struct VSOut { float4 pos : SV_Position; };
VSOut mainVS(uint id : SV_VertexID) {
    VSOut o;
    o.pos = float4(float(id & 1), float(id >> 1), 0.f, 1.f);
    return o;
}
)";

const char* kFragA = R"(// the tiny reload-test fragment stage
#include "common.hlsli"
struct VSOut { float4 pos : SV_Position; };
float4 mainPS(VSOut i) : SV_Target0 {
    return kTint;
}
)";

const char* kFragB = R"(// the EDITED reload-test fragment stage
#include "common.hlsli"
struct VSOut { float4 pos : SV_Position; };
float4 mainPS(VSOut i) : SV_Target0 {
    return kTint * 2.0;
}
)";

const char* kFragBroken = R"(// the BROKEN reload-test fragment stage
#include "common.hlsli"
struct VSOut { float4 pos : SV_Position; };
float4 mainPS(VSOut i) : SV_Target0 {
    return kTint * ; // syntax error
}
)";

const char* kInclude = R"(// the shared include (the .hlsli invalidation leg)
static const float4 kTint = float4(1.0, 0.0, 0.0, 1.0);
)";

std::vector<char> ReadBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteBytes(const fs::path& p, const char* text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

bool ValidSpirv(const fs::path& p) {
    const std::vector<char> b = ReadBytes(p);
    return !b.empty() && smvk::shaders::DetectBytecode(reinterpret_cast<const uint8_t*>(b.data()),
                                                       b.size()) == smvk::shaders::Bytecode::Spirv;
}

bool HasChange(const std::vector<smvk::shader_reload::Change>& log,
               const std::string& spvSuffix,
               smvk::shader_reload::Status status) {
    for (const auto& c : log)
        if (c.spvPath.find(spvSuffix) != std::string::npos && c.status == status)
            return true;
    return false;
}

void TestRecompile() {
    const fs::path tmp = fs::temp_directory_path() / "tourmaline_shreload";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    const fs::path src = tmp / "shaders";
    const fs::path out = tmp / "out";
    fs::create_directories(src, ec);
    fs::create_directories(out, ec);
    WriteBytes(src / "common.hlsli", kInclude);
    WriteBytes(src / "testonly.vert.hlsl", kVert);
    WriteBytes(src / "testonly.frag.hlsl", kFragA);

    std::string err;
    Check(smvk::shader_reload::Start(src.generic_string(), out.generic_string(), err),
          "reload.service-starts");

    // The base recompile: both stages land + validate.
    {
        smvk::shader_reload::Change c;
        Check(smvk::shader_reload::Recompile(
                  (src / "testonly.vert.hlsl").generic_string(), "testonly.vert.hlsl.spv", c) &&
                  c.status == smvk::shader_reload::Status::Recompiled &&
                  ValidSpirv(out / "testonly.vert.hlsl.spv"),
              "reload.vert-recompiles");
    }
    {
        smvk::shader_reload::Change c;
        Check(smvk::shader_reload::Recompile(
                  (src / "testonly.frag.hlsl").generic_string(), "testonly.frag.hlsl.spv", c) &&
                  c.status == smvk::shader_reload::Status::Recompiled &&
                  ValidSpirv(out / "testonly.frag.hlsl.spv"),
              "reload.frag-recompiles-includes");
    }
    const std::vector<char> before = ReadBytes(out / "testonly.frag.hlsl.spv");
    Check(!before.empty(), "reload.initial-bytes");

    // The byte-identical no-op: the same source skips the write.
    {
        smvk::shader_reload::Change c;
        Check(smvk::shader_reload::Recompile(
                  (src / "testonly.frag.hlsl").generic_string(), "testonly.frag.hlsl.spv", c) &&
                  c.status == smvk::shader_reload::Status::Recompiled,
              "reload.unchanged-noop");
    }

    // The EDIT: the bytes move.
    {
        WriteBytes(src / "testonly.frag.hlsl", kFragB);
        smvk::shader_reload::Change c;
        Check(smvk::shader_reload::Recompile(
                  (src / "testonly.frag.hlsl").generic_string(), "testonly.frag.hlsl.spv", c) &&
                  ReadBytes(out / "testonly.frag.hlsl.spv") != before,
              "reload.edit-moves-bytes");
    }
    const std::vector<char> good = ReadBytes(out / "testonly.frag.hlsl.spv");

    // THE LAST-KNOWN-GOOD CONTRACT: a broken source FAILS with the
    // compiler diagnostic + the previous .spv bytes are untouched.
    {
        WriteBytes(src / "testonly.frag.hlsl", kFragBroken);
        smvk::shader_reload::Change c;
        Check(!smvk::shader_reload::Recompile(
                  (src / "testonly.frag.hlsl").generic_string(), "testonly.frag.hlsl.spv", c) &&
                  c.status == smvk::shader_reload::Status::Failed && !c.diagnostic.empty() &&
                  ReadBytes(out / "testonly.frag.hlsl.spv") == good,
              "reload.broken-keeps-last-known-good");
    }

    // The variant twins: a file carrying the table's name compiles
    // its base + every twin (defines are macros — the tiny shader
    // need not use them).
    {
        WriteBytes(src / "default_lit.frag.hlsl", kFragA);
        smvk::shader_reload::Change c;
        Check(smvk::shader_reload::Recompile((src / "default_lit.frag.hlsl").generic_string(),
                                             "default_lit.frag.hlsl.spv",
                                             c) &&
                  ValidSpirv(out / "default_lit.frag.hlsl.spv") &&
                  ValidSpirv(out / "default_lit.frag.hlsl.sg5.spv") &&
                  ValidSpirv(out / "default_lit.frag.hlsl.gi3std.spv") &&
                  ValidSpirv(out / "default_lit.frag.hlsl.std.spv"),
              "reload.variant-twins-land");
    }

    // A non-shader-stage name refuses with the actionable reason.
    {
        WriteBytes(src / "notashader.hlsl", "float4 main() { return 0; }");
        smvk::shader_reload::Change c;
        Check(!smvk::shader_reload::Recompile(
                  (src / "notashader.hlsl").generic_string(), "notashader.spv", c) &&
                  c.status == smvk::shader_reload::Status::Failed &&
                  c.diagnostic.find("stage convention") != std::string::npos,
              "reload.non-stage-refused");
    }
    smvk::shader_reload::Stop();
}

void TestWatcher() {
    const fs::path tmp = fs::temp_directory_path() / "tourmaline_shreload_watch";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    const fs::path src = tmp / "shaders";
    const fs::path out = tmp / "out";
    fs::create_directories(src, ec);
    fs::create_directories(out, ec);
    WriteBytes(src / "common.hlsli", kInclude);
    WriteBytes(src / "testonly.vert.hlsl", kVert);
    WriteBytes(src / "testonly.frag.hlsl", kFragA);

    std::string err;
    Check(smvk::shader_reload::Start(src.generic_string(), out.generic_string(), err),
          "watch.starts");
    Check(smvk::shader_reload::Running(), "watch.running");
    // Quiet: no changes, no log.
    {
        const std::vector<smvk::shader_reload::Change> log =
            smvk::shader_reload::ConsumeChanges(40);
        Check(log.empty(), "watch.quiet-when-unchanged");
    }
    // A .hlsl edit lands through the watcher (settle + consume).
    WriteBytes(src / "testonly.frag.hlsl", kFragB);
    {
        std::vector<smvk::shader_reload::Change> log;
        for (int spin = 0; spin < 40 && log.empty(); ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            log = smvk::shader_reload::ConsumeChanges(40);
        }
        Check(HasChange(log, "testonly.frag.hlsl.spv", smvk::shader_reload::Status::Recompiled),
              "watch.hlsl-edit-recompiles");
        Check(ValidSpirv(out / "testonly.frag.hlsl.spv"), "watch.edited-spv-valid");
    }
    // A .hlsli edit invalidates EVERYTHING (the coarse include rule):
    // both .hlsl files recompile.
    {
        const std::string include2 = std::string(kInclude) + "\nstatic const float4 kTint2 = 1;\n";
        WriteBytes(src / "common.hlsli", include2.c_str());
        std::vector<smvk::shader_reload::Change> log;
        for (int spin = 0; spin < 40 && log.empty(); ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            log = smvk::shader_reload::ConsumeChanges(40);
        }
        Check(HasChange(log, "testonly.vert.hlsl.spv", smvk::shader_reload::Status::Recompiled) &&
                  HasChange(log, "testonly.frag.hlsl.spv", smvk::shader_reload::Status::Recompiled),
              "watch.hlsli-invalidates-all");
    }
    // A broken edit through the watcher: the FAILED change rides the
    // log, the good .spv stays.
    {
        const std::vector<char> good = ReadBytes(out / "testonly.frag.hlsl.spv");
        WriteBytes(src / "testonly.frag.hlsl", kFragBroken);
        std::vector<smvk::shader_reload::Change> log;
        for (int spin = 0; spin < 40 && log.empty(); ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            log = smvk::shader_reload::ConsumeChanges(40);
        }
        Check(HasChange(log, "testonly.frag.hlsl.spv", smvk::shader_reload::Status::Failed) &&
                  ReadBytes(out / "testonly.frag.hlsl.spv") == good,
              "watch.broken-fails-keeps-good");
    }
    smvk::shader_reload::Stop();
    Check(!smvk::shader_reload::Running(), "watch.stops");
    // ConsumeChanges after Stop is a safe no-op.
    Check(smvk::shader_reload::ConsumeChanges(10).empty(), "watch.post-stop-quiet");
}

// ---------------------------------------------------------------------------
// The CONSUMER-LEG coverage set (the live in-place swap): which changed
// .spv paths WorldRenderer::ReloadLitShaders rebuilds at the frame
// boundary. The classification is the contract between the service
// (which reports every recompiled path) and the consumer (which owns
// the scene lighting set + the GPU-driven legs): a path the classifier
// misses would keep serving last-known-good FOREVER (silently), and a
// path it over-covers costs a needless module rebuild. Pinned here so
// a new shader family/variant cannot drift the set.
// ---------------------------------------------------------------------------

void TestLiveSwapCoverage() {
    using engine::WorldRenderer;
    // The scene lighting set: the direct VS + every default_lit
    // permutation + the project family (base/a2c/sg/gi/std/giNstd).
    Check(WorldRenderer::LiveSwapCovers("content/shaders/default.vert.hlsl.spv"),
          "swap.cover-vert");
    Check(WorldRenderer::LiveSwapCovers("content/shaders/default_lit.frag.hlsl.spv"),
          "swap.cover-lit-base");
    Check(WorldRenderer::LiveSwapCovers("content/shaders/default_lit_a2c.frag.hlsl.spv"),
          "swap.cover-lit-a2c");
    Check(WorldRenderer::LiveSwapCovers("content/shaders/default_lit.frag.hlsl.sg5.spv") &&
              WorldRenderer::LiveSwapCovers("content/shaders/default_lit.frag.hlsl.sg12.spv"),
          "swap.cover-lit-sg-variants");
    Check(WorldRenderer::LiveSwapCovers("content/shaders/default_lit.frag.hlsl.gi3std.spv") &&
              WorldRenderer::LiveSwapCovers("content/shaders/default_lit.frag.hlsl.std.spv"),
          "swap.cover-lit-gi-std-variants");
    Check(WorldRenderer::LiveSwapCovers("content/shaders/project_lit.frag.hlsl.spv") &&
              WorldRenderer::LiveSwapCovers("content/shaders/project_lit_a2c.frag.hlsl.spv") &&
              WorldRenderer::LiveSwapCovers("content/shaders/project_lit.frag.hlsl.gi2.spv"),
          "swap.cover-project-family");
    // The GPU-driven legs: the filtered VS, the mesh MS + both mesh FS
    // families (stock + project twins, every variant).
    Check(WorldRenderer::LiveSwapCovers("content/shaders/gfxfiltered.vert.hlsl.spv"),
          "swap.cover-filtered-vs");
    Check(WorldRenderer::LiveSwapCovers("content/shaders/gpu_scene_meshlet.mesh.hlsl.spv"),
          "swap.cover-mesh-ms");
    Check(WorldRenderer::LiveSwapCovers("content/shaders/gfxmesh_lit.frag.hlsl.spv") &&
              WorldRenderer::LiveSwapCovers("content/shaders/gfxmesh_lit.frag.hlsl.sg9.spv"),
          "swap.cover-mesh-fs");
    Check(WorldRenderer::LiveSwapCovers("content/shaders/gfxmesh_project_lit.frag.hlsl.std.spv"),
          "swap.cover-mesh-project-fs");
    // The NOT-LIT set: these paths do not touch the LIT-family swap
    // (LiveSwapCovers false) — they belong to the PER-BUILDER leg
    // (ReloadBuilderShaders, which runs unconditionally on every
    // recompiled change set — the builders also consume lit modules
    // like default.vert, so the legs are not path-gated against each
    // other). The gfx compute chain (gfxfilter/gfxbatchcull/hzb) keeps
    // its last-known-good — the one remaining documented gap.
    Check(!WorldRenderer::LiveSwapCovers("content/shaders/default_unlit.frag.hlsl.spv"),
          "swap.cover-excludes-debug-fs");
    Check(!WorldRenderer::LiveSwapCovers("content/shaders/shadow.frag.hlsl.spv"),
          "swap.cover-excludes-shadow");
    Check(!WorldRenderer::LiveSwapCovers("content/shaders/gfxfilter.cs.hlsl.spv") &&
              !WorldRenderer::LiveSwapCovers("content/shaders/hzb_spd.cs.hlsl.min.spv"),
          "swap.cover-excludes-compute-chain");
    Check(!WorldRenderer::LiveSwapCovers("content/shaders/tonemap.frag.hlsl.spv") &&
              !WorldRenderer::LiveSwapCovers("content/shaders/bloom_down.frag.hlsl.spv") &&
              !WorldRenderer::LiveSwapCovers("content/shaders/ssr_compose.frag.hlsl.dbg.spv"),
          "swap.cover-excludes-postfx-builders");
    Check(!WorldRenderer::LiveSwapCovers("content/shaders/default_wire.frag.hlsl.spv"),
          "swap.cover-excludes-wire");
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;
    TestRecompile();
    TestWatcher();
    TestLiveSwapCoverage();
    std::cout << "shader_hot_reload_tests: " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
