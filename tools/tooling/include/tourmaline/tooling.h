// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Public CPU-side tooling API. All functions are callable in-process;
// no CLI invocation, shell commands, editor window or Vulkan device.
// Link CMake target tourmaline::tooling (STATIC).
// The project pipeline and engine are linked transitively by CMake.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tourmaline::tooling {

struct ProjectInitOptions {
    // Empty root defaults to current_directory()/name.
    std::filesystem::path root;
    std::string name;
    std::string recipeName = "game"; // basename, without .tourrecipe
    std::string pakName = "world"; // basename, without .pak
    std::string entryScene = "content/scenes/main.tour";
    bool bakeGi = false;
    bool bakeCubemaps = false;
    bool adoptExisting = false; // explicit opt-in; never overwrites an existing recipe
    bool dryRun = false;
    bool writeGitignore = true;
    std::filesystem::path fromGltf;  // optional existing glTF or GLB
    std::filesystem::path fromScene; // optional existing .tour; requires fromGltf
};

struct ProjectInitResult {
    std::string root;
    std::string name;
    std::string guid; // empty for dry runs
    std::string recipe; // absolute path
    std::string entryScene;
    bool created = false;
    bool imported = false;
    bool seededScene = false;
    bool recipeWritten = false;
    bool dryRun = false;
    bool readyToBuild = false; // false when entry has no actual source scene
};

// Refuses existing roots by default and never overwrites user-authored files.
// Uses project_pipeline's canonical Project::Init and import / scene seeding.
bool InitializeProject(const ProjectInitOptions& options,
                       ProjectInitResult& out, std::string& error);

struct AssetRecord {
    uint64_t id = 0;
    uint8_t kind = 0;
    std::string path;
    std::string scene;
    std::size_t mount = 0;
    bool operator==(const AssetRecord&) const = default;
};

struct PageRecord {
    std::size_t mount = 0;
    uint32_t kind = 0;
    std::string path;
    uint32_t slot = 0;
    uint64_t size = 0;
    uint32_t flags = 0;
    uint32_t contentCrc = 0;
    bool operator==(const PageRecord&) const = default;
};

struct PackageSnapshot {
    std::string source;
    std::vector<std::string> pakFiles; // physical names, intentionally excluded from equality
    std::vector<AssetRecord> assets;
    std::vector<PageRecord> pages;
    std::optional<std::size_t> entryMount;
    std::string entryScene;
};

// Opens only the SMP8 header/footer/TOC and the #ids page for each pak.
// No GPU and no whole-pak read. deepVerify hashes every stored page.
bool ReadPackageSnapshot(const std::filesystem::path& source,
                         PackageSnapshot& out, std::string& error,
                         bool deepVerify = false);

enum class DiffKind { Asset, Page, Entry };
enum class DiffStatus { Added, Removed, Modified };

struct Difference {
    DiffKind kind = DiffKind::Page;
    DiffStatus status = DiffStatus::Modified;
    std::string key; // asset: hex ID; page: <mount>/<kind>/<path>; entry: "entry"
    std::string path; // logical path (empty for entry)
    std::optional<AssetRecord> assetBefore, assetAfter;
    std::optional<PageRecord> pageBefore, pageAfter;
    std::string entryBefore, entryAfter;
};

struct DiffOptions {
    bool assets = true;
    bool pages = true;
    bool entry = true;
};

// Pure in-memory comparison; physical pak filenames are deliberately ignored.
std::vector<Difference> DiffPackageSnapshots(
    const PackageSnapshot& before, const PackageSnapshot& after,
    const DiffOptions& options = {});

// Rendering orchestration can use this reusable plan. This function does NOT
// launch a renderer or write files. Live execution remains a host decision.
struct CaptureOptions {
    std::filesystem::path executable;
    std::filesystem::path workingDirectory;
    std::string source; // .pak or .tapp relative to working directory
    uint32_t frames = 64;
    uint32_t captureFrame = 48;
    std::string cameraPosition;
    std::string cameraForward;
    std::map<std::string, std::string> environment;
};

struct CapturePlan {
    std::filesystem::path workingDirectory;
    std::vector<std::string> argv; // directly pass to a process launcher, never a shell
    std::map<std::string, std::string> environment; // overrides
};

bool BuildCapturePlan(const CaptureOptions& options,
                      CapturePlan& out, std::string& error);

// Decoded RGB pixel comparison kernel for host- or stb-decoded image buffers.
// 'stride' samples every N pixels horizontally and vertically.
struct PixelDifference {
    uint64_t sampled = 0;
    uint64_t differing = 0;
    double fraction = 0;
    double averageRgbChannelSum = 0;
};
bool CompareRgb8(std::span<const uint8_t> before, std::span<const uint8_t> after,
                 uint32_t width, uint32_t height, uint32_t threshold,
                 uint32_t stride, PixelDifference& out, std::string& error);

} // namespace tourmaline::tooling
