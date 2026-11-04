// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Deterministic import-time material channel packing.
// Decode is local to this TU (stb's symbols are static, matching
// gltf_import/tex_transcode.cpp and avoiding host symbol collisions).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <gltf_import/asset_export.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace smvk::assets {

bool ComposeMaterialImage(const std::filesystem::path& output,
    const std::array<PackedImageChannel, 4>& channels,
    std::string& err) {
    err.clear();
    struct Source {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
    };
    std::array<Source, 4> sources{};
    int width = 0, height = 0;
    for (size_t i = 0; i < channels.size(); ++i) {
        const auto& input = channels[i];
        if (input.component < 0 || input.component > 3) {
            err = "material packing channel outside RGBA";
            return false;
        }
        if (input.path.empty())
            continue;
        int w = 0, h = 0, n = 0;
        const std::string filename = input.path.string();
        stbi_uc* decoded = stbi_load(filename.c_str(), &w, &h, &n, 4);
        if (!decoded || w <= 0 || h <= 0) {
            err = "cannot decode material texture '" + filename + "': " +
                (stbi_failure_reason() ? stbi_failure_reason() : "unknown error");
            if (decoded) stbi_image_free(decoded);
            return false;
        }
        if (width == 0) { width = w; height = h; }
        if (w != width || h != height) {
            stbi_image_free(decoded);
            err = "packed material source dimensions differ: " + filename;
            return false;
        }
        const size_t pixels = size_t(w) * size_t(h);
        if (pixels > std::numeric_limits<size_t>::max() / 4u) {
            stbi_image_free(decoded);
            err = "packed material source exceeds addressable image size";
            return false;
        }
        sources[i].rgba.assign(decoded, decoded + pixels * 4u);
        sources[i].width = w;
        sources[i].height = h;
        stbi_image_free(decoded);
    }
    if (width == 0) { width = 1; height = 1; }
    if (size_t(width) * size_t(height) > std::numeric_limits<size_t>::max() / 4u) {
        err = "packed material destination exceeds addressable image size";
        return false;
    }
    const size_t pixelCount = size_t(width) * size_t(height);
    std::vector<uint8_t> out(pixelCount * 4u);
    for (size_t pixel = 0; pixel < pixelCount; ++pixel)
        for (size_t c = 0; c < 4; ++c)
            out[pixel * 4 + c] = channels[c].path.empty()
                ? channels[c].constant
                : sources[c].rgba[pixel * 4 + size_t(channels[c].component)];
    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
        err = "cannot create packed material directory: " + ec.message();
        return false;
    }
    if (!stbi_write_png(output.string().c_str(), width, height, 4,
            out.data(), width * 4)) {
        err = "could not write packed material PNG: " + output.string();
        return false;
    }
    return true;
}

} // namespace smvk::assets
