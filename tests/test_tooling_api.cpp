// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPU-free contracts for the composable native tooling library.
#include <tourmaline/tooling.h>
#include <project_pipeline/project.h>
#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/pak.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

int failures = 0;
void Check(bool ok, const char* description) {
    if (!ok) {
        std::cerr << "FAIL: " << description << "\n";
        ++failures;
    }
}
}

int main() {
    using namespace tourmaline::tooling;
    std::string error;

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path()
        / ("tourmaline-tooling-" + std::to_string(nonce));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); }
    } cleanup{root};

    ProjectInitOptions opts;
    opts.root = root;
    opts.name = "My First Project";
    opts.recipeName = "sandbox";
    opts.entryScene = "content/scenes/entry.tour";
    opts.bakeCubemaps = true;
    opts.dryRun = true;
    ProjectInitResult result;
    Check(InitializeProject(opts, result, error), "dry-run reports success");
    Check(result.dryRun && !fs::exists(root), "dry-run does not touch disk");

    opts.dryRun = false;
    Check(InitializeProject(opts, result, error), "project initialized");
    Check(result.created && result.guid.size() == 32,
          "canonical marker GUID minted exactly once");
    Check(fs::is_regular_file(root / "project.tourproj"), "project marker exists");
    Check(fs::is_regular_file(root / "sandbox.tourrecipe"), "recipe exists");
    Check(fs::is_directory(root / "content/scenes"), "content directories created");
    Check(!result.readyToBuild, "does not invent a non-existent scene");

    const std::string originalGuid = result.guid;
    const std::string originalRecipe = [] (const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }(root / "sandbox.tourrecipe");
    Check(!InitializeProject(opts, result, error), "unapproved re-init refused");
    opts.adoptExisting = true;
    Check(InitializeProject(opts, result, error), "explicit adopt allowed");
    Check(!result.created && originalGuid == result.guid, "adopt preserves identity");
    Check(!result.recipeWritten, "adopt does not overwrite recipe");
    Check(originalRecipe == [] (const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }(root / "sandbox.tourrecipe"), "recipe bytes preserved");

    // Real file-backed SMP8 reader smoke test: this checks the shared
    // engine's TOC parsing and the #ids parser, not a duplicate reader.
    tour::asset::PakWriter writer;
    const std::vector<tour::asset::PakAssetIdRow> ids = {
        {0x1001, 1, "content/scenes/main.tour", "main"}
    };
    Check(writer.AddBlob(tour::asset::PakAssetKind::Identity,
                         "#ids", tour::asset::WriteAssetIdsBlob(ids)),
          "identity page added to synthetic SMP8 pak");
    Check(writer.AddBlob(tour::asset::PakAssetKind::Sound,
                         "content/audio/tone.wav", {82, 73, 70, 70}),
          "sound page added to synthetic SMP8 pak");
    const fs::path pakPath = root / "world-1234.pak";
    const auto bytes = writer.Finalize();
    {
        std::ofstream stream(pakPath, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    smvk::AppManifest manifest;
    manifest.version = smvk::kAppManifestVersion;
    manifest.entryPak = "world-1234.pak";
    manifest.entryScene = "main";
    manifest.paks = {"world-1234.pak"};
    const fs::path manifestPath = root / "sample.tapp";
    Check(smvk::SaveAppManifest(manifestPath.generic_string(), manifest, error),
          "synthetic manifest written");
    PackageSnapshot opened;
    Check(ReadPackageSnapshot(manifestPath, opened, error, true),
          "read package snapshot from real engine reader with deep CRC");
    Check(opened.assets.size() == 1 && opened.assets[0].id == 0x1001,
          "reader exposes persistent #ids identity");
    Check(opened.pages.size() == 2 && opened.entryMount == 0,
          "reader exposes physical page and entry mount");

    PackageSnapshot before, after;
    before.source = "old.tapp";
    before.pakFiles = {"world-aa11.pak"};
    before.assets = {{0x1001, 3, "models/rock", "", 0}};
    before.pages = {{0, 4, "textures/base.ktx", 1, 200, 0, 123}};
    before.entryMount = 0;
    before.entryScene = "main";
    after = before;
    after.source = "new.tapp";
    after.pakFiles = {"world-bbbb.pak"};
    Check(DiffPackageSnapshots(before, after).empty(),
          "content-addressed filename changes do not count");
    after.pages[0].contentCrc = 999;
    auto differences = DiffPackageSnapshots(before, after);
    Check(differences.size() == 1 &&
          differences[0].kind == DiffKind::Page &&
          differences[0].status == DiffStatus::Modified,
          "page CRC metadata differences");
    DiffOptions assetsOnly;
    assetsOnly.pages = false;
    Check(DiffPackageSnapshots(before, after, assetsOnly).empty(),
          "physical CRC difference is not invented as asset-ID change");
    after.assets[0].path = "models/other";
    Check(DiffPackageSnapshots(before, after, assetsOnly).size() == 1,
          "logical identity metadata differences");

    CaptureOptions capture;
    capture.executable = "out/build/App";
    capture.source = "content/paks/game.tapp";
    capture.frames = 64;
    capture.captureFrame = 48;
    capture.cameraPosition = "0,2,-4";
    capture.cameraForward = "0,0,1";
    CapturePlan plan;
    Check(BuildCapturePlan(capture, plan, error), "capture launch plan built");
    Check(plan.environment.at("TOURMALINE_CAPTURE") == "48",
          "capture frame pinned");
    capture.captureFrame = 64;
    Check(!BuildCapturePlan(capture, plan, error), "out-of-budget frame refused");

    std::array<uint8_t, 12> black{};
    std::array<uint8_t, 12> changed{};
    changed[3] = 255;
    PixelDifference stats;
    Check(CompareRgb8(black, changed, 2, 2, 6, 1, stats, error),
          "RGB8 comparison accepted");
    Check(stats.sampled == 4 && stats.differing == 1 &&
          stats.fraction == 0.25, "RGB8 threshold statistics");

    if (failures) {
        std::cerr << failures << " tooling checks failed\n";
        return 1;
    }
    std::cout << "tooling API checks passed\n";
    return 0;
}
