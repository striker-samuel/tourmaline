// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <tourmaline/tooling.h>

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/pak.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>

namespace tourmaline::tooling {
namespace {

namespace fs = std::filesystem;
using Key = std::tuple<std::size_t, uint32_t, std::string>;

std::string AssetKey(uint64_t id) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << id;
    return os.str();
}

std::string PageKey(const PageRecord& row) {
    return std::to_string(row.mount) + "/" + std::to_string(row.kind) +
           "/" + row.path;
}

bool ValidVector(const std::string& text) {
    std::istringstream in(text);
    for (int i = 0; i != 3; ++i) {
        double n = 0;
        if (!(in >> n) || !std::isfinite(n))
            return false;
        if (i != 2) {
            char comma = 0;
            if (!(in >> comma) || comma != ',')
                return false;
        }
    }
    in >> std::ws;
    return in.eof();
}

} // namespace

bool ReadPackageSnapshot(const fs::path& source, PackageSnapshot& out,
                         std::string& error, bool deepVerify) {
    error.clear();
    PackageSnapshot result;
    std::error_code ec;
    const fs::path absolute = fs::absolute(source, ec).lexically_normal();
    if (ec || absolute.empty()) {
        error = "cannot resolve source path";
        return false;
    }
    result.source = absolute.generic_string();

    smvk::AppManifest manifest;
    bool isManifest = absolute.extension() == ".tapp";
    std::vector<fs::path> pakPaths;
    if (isManifest) {
        if (!smvk::LoadAppManifest(result.source, manifest, error))
            return false;
        // LoadAppManifest validates relative, non-escaping manifest paths.
        for (const std::string& name : manifest.paks)
            pakPaths.push_back(absolute.parent_path() / fs::path(name));
        auto entry = std::find(manifest.paks.begin(), manifest.paks.end(), manifest.entryPak);
        if (entry == manifest.paks.end()) {
            error = "manifest entry pak not in ordered mount list";
            return false;
        }
        result.entryMount = static_cast<std::size_t>(entry - manifest.paks.begin());
        result.entryScene = manifest.entryScene;
    } else if (absolute.extension() == ".pak") {
        pakPaths.push_back(absolute);
    } else {
        error = "expected .pak or .tapp source";
        return false;
    }

    std::map<uint64_t, std::size_t> seenAssets;
    for (std::size_t mount = 0; mount < pakPaths.size(); ++mount) {
        const fs::path path = pakPaths[mount];
        result.pakFiles.push_back(path.filename().generic_string());
        std::string openError;
        auto reader = tour::asset::PakReader::OpenFile(path.generic_string(), openError);
        if (!reader) {
            error = path.generic_string() + ": " + openError;
            return false;
        }

        for (const tour::asset::PakTocEntry& entry : reader->Entries()) {
            result.pages.push_back(PageRecord{
                mount,
                static_cast<uint32_t>(entry.kind),
                entry.path,
                entry.slot,
                entry.size,
                entry.flags,
                entry.contentCrc
            });
        }

        const auto* ids = reader->Find(tour::asset::PakAssetKind::Identity,
                                        tour::asset::kPakAssetIdsPath);
        if (ids) {
            std::span<const uint8_t> blob;
            try {
                blob = reader->Payload(*ids);
            } catch (const std::exception& exc) {
                error = path.generic_string() + ": #ids read failed: " + exc.what();
                return false;
            }
            if (blob.empty()) {
                error = path.generic_string() + ": cannot read or verify #ids page";
                return false;
            }
            std::vector<tour::asset::PakAssetIdRow> parsed;
            if (!tour::asset::ParseAssetIdsBlob(blob, parsed, error)) {
                error = path.generic_string() + ": invalid #ids: " + error;
                return false;
            }
            for (const auto& row : parsed) {
                if (const auto prior = seenAssets.find(row.id); prior != seenAssets.end()) {
                    error = "duplicate asset " + AssetKey(row.id) + " in mounts " +
                            std::to_string(prior->second) + " and " + std::to_string(mount);
                    return false;
                }
                seenAssets.emplace(row.id, mount);
                result.assets.push_back(AssetRecord{
                    row.id, row.kind, row.path, row.sceneName, mount
                });
            }
        } else if (isManifest) {
            error = path.generic_string() + ": appmanifest 2 requires #ids";
            return false;
        }

        if (deepVerify && !reader->VerifyAll(error)) {
            error = path.generic_string() + ": failed stored-page CRC: " + error;
            return false;
        }
    }
    std::sort(result.assets.begin(), result.assets.end(),
              [](const AssetRecord& a, const AssetRecord& b) {
                  return a.id < b.id;
              });
    std::sort(result.pages.begin(), result.pages.end(),
              [](const PageRecord& a, const PageRecord& b) {
                  return std::tie(a.mount, a.kind, a.path) <
                         std::tie(b.mount, b.kind, b.path);
              });
    out = std::move(result); // publish only after the entire release opens
    return true;
}

std::vector<Difference> DiffPackageSnapshots(const PackageSnapshot& before,
                                              const PackageSnapshot& after,
                                              const DiffOptions& options) {
    std::vector<Difference> result;
    if (options.assets) {
        std::map<uint64_t, AssetRecord> left, right;
        for (const auto& asset : before.assets) left[asset.id] = asset;
        for (const auto& asset : after.assets) right[asset.id] = asset;
        for (const auto& [id, old] : left) {
            auto found = right.find(id);
            if (found == right.end() || !(old == found->second)) {
                Difference row;
                row.kind = DiffKind::Asset;
                row.status = found == right.end() ? DiffStatus::Removed : DiffStatus::Modified;
                row.key = AssetKey(id);
                row.path = old.path;
                row.assetBefore = old;
                if (found != right.end()) {
                    row.path = found->second.path;
                    row.assetAfter = found->second;
                }
                result.push_back(std::move(row));
            }
        }
        for (const auto& [id, asset] : right) {
            if (left.count(id)) continue;
            Difference row;
            row.kind = DiffKind::Asset;
            row.status = DiffStatus::Added;
            row.key = AssetKey(id);
            row.path = asset.path;
            row.assetAfter = asset;
            result.push_back(std::move(row));
        }
    }
    if (options.pages) {
        std::map<Key, PageRecord> left, right;
        const auto key = [](const PageRecord& p) {
            return Key(p.mount, p.kind, p.path);
        };
        for (const auto& page : before.pages) left[key(page)] = page;
        for (const auto& page : after.pages) right[key(page)] = page;
        for (const auto& [pageKey, old] : left) {
            auto found = right.find(pageKey);
            if (found == right.end() || !(old == found->second)) {
                Difference row;
                row.kind = DiffKind::Page;
                row.status = found == right.end() ? DiffStatus::Removed : DiffStatus::Modified;
                row.key = PageKey(old);
                row.path = old.path;
                row.pageBefore = old;
                if (found != right.end()) row.pageAfter = found->second;
                result.push_back(std::move(row));
            }
        }
        for (const auto& [pageKey, page] : right) {
            if (left.count(pageKey)) continue;
            Difference row;
            row.kind = DiffKind::Page;
            row.status = DiffStatus::Added;
            row.key = PageKey(page);
            row.path = page.path;
            row.pageAfter = page;
            result.push_back(std::move(row));
        }
    }
    if (options.entry &&
            (before.entryMount != after.entryMount || before.entryScene != after.entryScene)) {
        Difference row;
        row.kind = DiffKind::Entry;
        row.status = DiffStatus::Modified;
        row.key = "entry";
        auto entryText = [](const PackageSnapshot& s) {
            return s.entryMount
                ? std::to_string(*s.entryMount) + "/" + s.entryScene
                : std::string("none");
        };
        row.entryBefore = entryText(before);
        row.entryAfter = entryText(after);
        result.push_back(std::move(row));
    }
    return result;
}

bool BuildCapturePlan(const CaptureOptions& options,
                      CapturePlan& out, std::string& error) {
    error.clear();
    if (options.executable.empty() || options.source.empty()) {
        error = "capture requires executable and .pak/.tapp source";
        return false;
    }
    if (options.frames < 2 || options.captureFrame == 0 ||
        options.captureFrame >= options.frames) {
        error = "require 1 <= captureFrame < frames";
        return false;
    }
    const fs::path source(options.source);
    if (source.extension() != ".pak" && source.extension() != ".tapp") {
        error = "capture source must have .pak or .tapp extension";
        return false;
    }
    if (options.cameraPosition.empty() != options.cameraForward.empty()) {
        error = "set both camera position and direction, or neither";
        return false;
    }
    if (!options.cameraPosition.empty() &&
        (!ValidVector(options.cameraPosition) || !ValidVector(options.cameraForward))) {
        error = "camera vectors require three finite comma-separated numbers";
        return false;
    }
    CapturePlan plan;
    plan.workingDirectory = options.workingDirectory.empty()
        ? options.executable.parent_path() : options.workingDirectory;
    plan.argv = {options.executable.generic_string(),
                 source.extension() == ".pak" ? "--pak" : "--project",
                 options.source, "--disable_control",
                 "--bench", std::to_string(options.frames),
                 "--present", "immediate"};
    const std::regex allowed(R"(^TOURMALINE_[A-Z0-9_]+$)");
    for (const auto& [name, value] : options.environment) {
        if (!std::regex_match(name, allowed) || name == "TOURMALINE_CAPTURE" ||
            value.find('\0') != std::string::npos ||
            value.find('\n') != std::string::npos) {
            error = "invalid or reserved capture environment variable: " + name;
            return false;
        }
    }
    plan.environment["TOURMALINE_AA"] = "off";
    plan.environment["TOURMALINE_AUTO_EXPOSURE"] = "0";
    for (const auto& [name, value] : options.environment)
        plan.environment[name] = value;
    plan.environment["TOURMALINE_CAPTURE"] = std::to_string(options.captureFrame);
    if (!options.cameraPosition.empty()) {
        plan.environment["TOURMALINE_CAM_POS"] = options.cameraPosition;
        plan.environment["TOURMALINE_CAM_FWD"] = options.cameraForward;
    }
    out = std::move(plan);
    return true;
}

bool CompareRgb8(std::span<const uint8_t> before, std::span<const uint8_t> after,
                 uint32_t width, uint32_t height, uint32_t threshold,
                 uint32_t stride, PixelDifference& out, std::string& error) {
    error.clear();
    if (width == 0 || height == 0 || stride == 0 || threshold > 765) {
        error = "invalid dimensions, stride, or threshold";
        return false;
    }
    const uint64_t pixels = uint64_t(width) * height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3 ||
        before.size() != pixels * 3 || after.size() != pixels * 3) {
        error = "expected identical RGB8 images of width*height*3 bytes";
        return false;
    }
    PixelDifference result{};
    uint64_t sum = 0;
    for (uint32_t y = 0; y < height; y += stride) {
        for (uint32_t x = 0; x < width; x += stride) {
            const auto offset = (std::size_t(y) * width + x) * 3;
            int delta = 0;
            for (int c = 0; c < 3; ++c) {
                delta += std::abs(int(before[offset + c]) - int(after[offset + c]));
            }
            ++result.sampled;
            if (uint32_t(delta) > threshold) ++result.differing;
            sum += uint64_t(delta);
            // Avoid overflow on strides near uint32_t max.
            if (stride > width - 1 - x) break;
        }
        if (stride > height - 1 - y) break;
    }
    result.fraction = double(result.differing) / result.sampled;
    result.averageRgbChannelSum = double(sum) / result.sampled;
    out = result;
    return true;
}

} // namespace tourmaline::tooling
