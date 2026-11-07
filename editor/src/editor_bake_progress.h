// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Decode the baker's existing human-readable progress lines without making
// the editor depend on the GI implementation. Fractions describe the CURRENT
// phase (trace chunks/probes), never the entire multi-stage cook.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace editor {

struct BakeProgressUpdate {
    bool recognized = false;
    std::string stage;
    float fraction = -1.0f; // indeterminate when the current phase has no total
};

namespace detail {
inline bool ProgressCounts(std::string_view line,
                           std::string_view prefix,
                           std::string_view unit,
                           uint64_t& done,
                           uint64_t& total) {
    if (!line.starts_with(prefix))
        return false;
    line.remove_prefix(prefix.size());
    auto parse = [&line](uint64_t& value) {
        const auto* first = line.data();
        const auto* last = first + line.size();
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || ptr == first)
            return false;
        line.remove_prefix(size_t(ptr - first));
        return true;
    };
    if (!parse(done) || line.empty() || line.front() != '/')
        return false;
    line.remove_prefix(1);
    return parse(total) && total != 0 && line.starts_with(unit);
}
} // namespace detail

inline BakeProgressUpdate ParseBakeProgress(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    BakeProgressUpdate u{};
    uint64_t done = 0, total = 0;
    if (detail::ProgressCounts(line, "lightmap bake: ", " chunks", done, total)) {
        u.recognized = true;
        u.stage =
            "Lightmap tracing: " + std::to_string(done) + "/" + std::to_string(total) + " chunks";
        u.fraction = float(std::min(done, total)) / float(total);
        return u;
    }
    if (detail::ProgressCounts(line, "givolume bake: ", " probes", done, total)) {
        u.recognized = true;
        u.stage =
            "GI volume tracing: " + std::to_string(done) + "/" + std::to_string(total) + " probes";
        u.fraction = float(std::min(done, total)) / float(total);
        return u;
    }
    if (line.starts_with("lightmap bake: done:"))
        return {true, "Lightmap tracing complete", 1.0f};
    if (line.starts_with("givolume bake: done:"))
        return {true, "GI volume tracing complete", 1.0f};
    if (line.starts_with("lightmap bake: ") && line.find(" chunks, ") != std::string_view::npos)
        return {true, "Lightmap tracing (preparing chunks)...", 0.0f};
    if (line.starts_with("givolume bake: layout"))
        return {true, "GI volume: preparing probe layout...", -1.0f};
    if (line.starts_with("[GIV] volume '"))
        return {true, std::string(line), -1.0f};
    if (!line.starts_with("[LM] "))
        return u;
    if (line.find("proxy:") != std::string_view::npos ||
        line.find("proxy charted:") != std::string_view::npos)
        return {true, "Lightmap: generating proxy charts...", -1.0f};
    if (line.find("xatlas") != std::string_view::npos ||
        line.find("atlas round") != std::string_view::npos)
        return {true, "Lightmap: packing UV charts...", -1.0f};
    if (line.find("seam stitch") != std::string_view::npos)
        return {true, "Lightmap: stitching chart seams...", -1.0f};
    if (line.find("bc6h") != std::string_view::npos)
        return {true, "Lightmap: compressing atlas...", -1.0f};
    if (line.find("atlas ") != std::string_view::npos)
        return {true, "Lightmap: writing atlas...", -1.0f};
    if (line.find("lightmaps:") != std::string_view::npos)
        return {true, "Lightmap atlas finished", 1.0f};
    return u;
}

} // namespace editor
