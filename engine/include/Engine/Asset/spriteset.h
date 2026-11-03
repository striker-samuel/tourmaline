// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// SpriteSet is a CPU-only authored atlas catalog. No texture ownership and
// no renderer dependency: UI and world sprites can share the same metadata.
// All rectangles use TOP-LEFT pixel coordinates in the original texture.
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tour::asset {

struct SpriteRect {
    uint32_t x = 0, y = 0, width = 0, height = 0;
};

struct SpriteRegion {
    std::string name;
    SpriteRect rect;
    // Normalized pivot relative to the UNTRIMMED source dimensions.
    float pivotX = 0.5f, pivotY = 0.5f;
    uint32_t sourceWidth = 0, sourceHeight = 0;
    uint32_t trimX = 0, trimY = 0;
    // Nine-slice insets, relative to the sprite rectangle: L, T, R, B.
    std::array<uint32_t, 4> border{};
};

struct SpriteAnimation {
    std::string name;
    float fps = 1.0f;
    bool loop = true;
    // Indices into SpriteSet::sprites (all names validated at load time).
    std::vector<uint32_t> frames;

    [[nodiscard]] uint32_t FrameAt(float seconds) const {
        if (frames.empty() || !std::isfinite(seconds) || seconds <= 0)
            return frames.empty() ? UINT32_MAX : frames.front();
        const double n = std::floor(double(seconds) * double(fps));
        const size_t index = loop
            ? static_cast<size_t>(std::fmod(n, double(frames.size())))
            : (n >= double(frames.size()) ? frames.size() - 1 : size_t(n));
        return frames[index];
    }
};

struct SpriteSet {
    std::string texture; // project-relative VFS path, separately cataloged
    uint32_t width = 0, height = 0;
    std::vector<SpriteRegion> sprites;
    std::vector<SpriteAnimation> animations;

    [[nodiscard]] const SpriteRegion* Find(std::string_view name) const {
        for (const auto& s : sprites)
            if (s.name == name) return &s;
        return nullptr;
    }
    [[nodiscard]] const SpriteAnimation* FindAnimation(
        std::string_view name) const {
        for (const auto& a : animations)
            if (a.name == name) return &a;
        return nullptr;
    }
    // x/y/w/h in UV units, without padding or half-texel contraction.
    [[nodiscard]] std::array<float, 4> UV(const SpriteRegion& s) const {
        if (!width || !height) return {};
        return {float(s.rect.x) / width, float(s.rect.y) / height,
            float(uint64_t(s.rect.x) + s.rect.width) / width,
            float(uint64_t(s.rect.y) + s.rect.height) / height};
    }
};

namespace sprite_detail {
inline bool Integer(std::string_view token, uint32_t& out) {
    if (token.empty()) return false;
    const char* first = token.data();
    const auto result = std::from_chars(first, first + token.size(), out);
    return result.ec == std::errc{} && result.ptr == first + token.size();
}
inline bool Real(const std::string& token, float& out) {
    std::istringstream in(token);
    in >> out;
    return bool(in) && in.eof() && std::isfinite(out);
}
inline bool Name(std::string_view s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')
            && !(c >= '0' && c <= '9') && c != '_' && c != '-')
            return false;
    return true;
}
inline bool Path(std::string_view s) {
    if (s.empty() || s[0] == '/' || s.find('\\') != s.npos
        || s.find(':') != s.npos || s.find("//") != s.npos)
        return false;
    for (size_t i = 0; i < s.size();) {
        const size_t e = s.find('/', i);
        const auto segment = s.substr(i,
            e == s.npos ? s.npos : e - i);
        if (segment == "." || segment == ".." || segment.empty())
            return false;
        if (e == s.npos) break;
        i = e + 1;
    }
    return true;
}
inline bool Inside(SpriteRect r, uint32_t w, uint32_t h) {
    return r.width && r.height && uint64_t(r.x) + r.width <= w
        && uint64_t(r.y) + r.height <= h;
}
inline bool Valid(const SpriteRegion& s, uint32_t w, uint32_t h) {
    if (!Inside(s.rect, w, h) || !std::isfinite(s.pivotX)
        || !std::isfinite(s.pivotY) || s.pivotX < 0 || s.pivotX > 1
        || s.pivotY < 0 || s.pivotY > 1
        || !s.sourceWidth || !s.sourceHeight
        || uint64_t(s.trimX) + s.rect.width > s.sourceWidth
        || uint64_t(s.trimY) + s.rect.height > s.sourceHeight)
        return false;
    const auto& b = s.border;
    if (uint64_t(b[0]) + b[2] > s.rect.width
        || uint64_t(b[1]) + b[3] > s.rect.height)
        return false;
    // Nine-slice and cropping are incompatible: borders describe
    // the untrimmed sprite, not an arbitrarily cropped alpha region.
    if ((b[0] || b[1] || b[2] || b[3])
        && (s.trimX || s.trimY || s.sourceWidth != s.rect.width
            || s.sourceHeight != s.rect.height))
        return false;
    return true;
}
} // namespace sprite_detail

// Versioned, strictly validated, line-oriented UTF-8 ASCII tokens:
//   spriteset 1
//   texture content/ui/atlas.png
//   size 128 64
//   sprite button 0 0 32 32 pivot 0.5 0.5 border 6 6 6 6
//   sprite walk0 32 0 32 30 pivot 0.5 1 trim 32 32 0 2
//   grid frame 0 32 4 1 32 32 pivot 0.5 1
//   anim walk 10 loop frame_0 frame_1 frame_2 frame_3
//
// Grid names are prefix_<row-major index>. Animation names resolve after
// parsing, so forward references are legal. Unknown/duplicate fields fail.
inline bool ParseSpriteSetText(std::string_view text, SpriteSet& out,
    std::string& err) {
    err.clear();
    SpriteSet result;
    std::unordered_map<std::string, uint32_t> names;
    struct PendingAnimation {
        std::string name;
        float fps = 1.0f;
        bool loop = true;
        std::vector<std::string> frames;
    };
    std::vector<PendingAnimation> pending;
    bool header = false, hasTexture = false, hasSize = false;
    uint32_t lineNo = 0;
    auto fail = [&](const std::string& why) {
        err = "spriteset line " + std::to_string(lineNo) + ": " + why;
        return false;
    };
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        ++lineNo;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream in(line);
        std::string verb;
        if (!(in >> verb)) continue;
        std::vector<std::string> tok;
        for (std::string t; in >> t;) tok.push_back(std::move(t));
        if (!header) {
            if (verb != "spriteset" || tok.size() != 1 || tok[0] != "1")
                return fail("expected spriteset 1");
            header = true;
            continue;
        }
        if (verb == "texture") {
            if (hasTexture || tok.size() != 1
                || !sprite_detail::Path(tok[0]))
                return fail("expected a single relative texture path");
            result.texture = tok[0];
            hasTexture = true;
        } else if (verb == "size") {
            if (hasSize || tok.size() != 2
                || !sprite_detail::Integer(tok[0], result.width)
                || !sprite_detail::Integer(tok[1], result.height)
                || !result.width || !result.height)
                return fail("expected nonzero texture size W H");
            hasSize = true;
        } else if (verb == "sprite" || verb == "grid") {
            if (!hasSize) return fail("size must precede sprite/grid");
            const bool grid = verb == "grid";
            if (tok.size() < (grid ? 7u : 5u)
                || !sprite_detail::Name(tok[0]))
                return fail("invalid region name or coordinates");
            uint32_t numbers[6]{};
            const size_t n = grid ? 6 : 4;
            for (size_t j = 0; j < n; ++j)
                if (!sprite_detail::Integer(tok[j + 1], numbers[j]))
                    return fail("region coordinates must be uint32");
            SpriteRegion proto;
            proto.name = tok[0];
            proto.rect = {numbers[0], numbers[1],
                grid ? numbers[4] : numbers[2],
                grid ? numbers[5] : numbers[3]};
            proto.sourceWidth = proto.rect.width;
            proto.sourceHeight = proto.rect.height;
            for (size_t i = n + 1; i < tok.size();) {
                const std::string& option = tok[i++];
                if (option == "pivot") {
                    if (i + 2 > tok.size()
                        || !sprite_detail::Real(tok[i], proto.pivotX)
                        || !sprite_detail::Real(tok[i + 1], proto.pivotY))
                        return fail("pivot requires two finite floats");
                    i += 2;
                } else if (option == "trim" && !grid) {
                    if (i + 4 > tok.size()
                        || !sprite_detail::Integer(tok[i], proto.sourceWidth)
                        || !sprite_detail::Integer(tok[i + 1], proto.sourceHeight)
                        || !sprite_detail::Integer(tok[i + 2], proto.trimX)
                        || !sprite_detail::Integer(tok[i + 3], proto.trimY))
                        return fail("trim requires sourceW sourceH offsetX offsetY");
                    i += 4;
                } else if (option == "border" && !grid) {
                    if (i + 4 > tok.size()) return fail("border needs L T R B");
                    for (size_t j = 0; j < 4; ++j)
                        if (!sprite_detail::Integer(tok[i + j], proto.border[j]))
                            return fail("border must contain four uint32 values");
                    i += 4;
                } else return fail("unknown or disallowed region option: " + option);
            }
            const uint32_t columns = grid ? numbers[2] : 1;
            const uint32_t rows = grid ? numbers[3] : 1;
            if (!columns || !rows
                || uint64_t(columns) * rows >
                    65536ull - result.sprites.size())
                return fail("grid count is empty or too large");
            if (grid && (uint64_t(proto.rect.x) +
                    uint64_t(columns) * proto.rect.width > result.width
                || uint64_t(proto.rect.y) +
                    uint64_t(rows) * proto.rect.height > result.height))
                return fail("grid exceeds texture bounds");
            for (uint32_t y = 0; y < rows; ++y)
                for (uint32_t x = 0; x < columns; ++x) {
                    SpriteRegion s = proto;
                    if (grid) {
                        s.name += "_" + std::to_string(uint64_t(y) * columns + x);
                        s.rect.x += x * proto.rect.width;
                        s.rect.y += y * proto.rect.height;
                    }
                    if (!sprite_detail::Valid(s, result.width, result.height))
                        return fail("sprite bounds, trim, pivot or border invalid");
                    if (!names.emplace(s.name,
                            static_cast<uint32_t>(result.sprites.size())).second)
                        return fail("duplicate sprite name: " + s.name);
                    result.sprites.push_back(std::move(s));
                }
        } else if (verb == "anim") {
            if (tok.size() < 4 || !sprite_detail::Name(tok[0]))
                return fail("anim requires name fps loop|once frames...");
            PendingAnimation a;
            a.name = tok[0];
            if (!sprite_detail::Real(tok[1], a.fps)
                || a.fps <= 0 || (tok[2] != "loop" && tok[2] != "once"))
                return fail("invalid animation fps or playback");
            a.loop = tok[2] == "loop";
            a.frames.assign(tok.begin() + 3, tok.end());
            if (std::any_of(pending.begin(), pending.end(),
                [&](const PendingAnimation& b) { return b.name == a.name; }))
                return fail("duplicate animation name: " + a.name);
            pending.push_back(std::move(a));
        } else return fail("unknown directive: " + verb);
    }
    if (!header || !hasTexture || !hasSize) return fail(
        "missing spriteset 1, texture or size directive");
    for (const auto& a : pending) {
        SpriteAnimation anim;
        anim.name = a.name;
        anim.fps = a.fps;
        anim.loop = a.loop;
        for (const auto& name : a.frames) {
            auto it = names.find(name);
            if (it == names.end()) return fail(
                "animation '" + a.name + "' references unknown sprite: " + name);
            anim.frames.push_back(it->second);
        }
        result.animations.push_back(std::move(anim));
    }
    out = std::move(result); // transactional: failed parses never change out
    return true;
}

} // namespace tour::asset
