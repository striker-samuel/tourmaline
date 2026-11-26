// SPDX-License-Identifier: MIT
#pragma once

// CPU reference for the proposed GPU cache; no renderer dependencies.
// Cache incoming diffuse irradiance (not albedo-modulated outgoing radiance).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace smvk::sdfgi {
using Vec3 = std::array<float, 3>;

inline float Dot(const Vec3& a, const Vec3& b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
inline Vec3 Sub(const Vec3& a, const Vec3& b) {
    return {a[0]-b[0], a[1]-b[1], a[2]-b[2]};
}
inline bool Finite(const Vec3& a) {
    return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]);
}

// Positions are reconstructed from depth; normals are unit geometric normals.
// Split by plane separation, not raw depth range (sloped surfaces stay together).
struct Surface {
    Vec3 position{}, normal{};
    uint64_t instance = 0;
};
inline bool Compatible(const Surface& a, const Surface& b, float planeTolerance,
                       float normalCos = 0.8f) {
    if (!Finite(a.position) || !Finite(b.position) || !Finite(a.normal) ||
        !Finite(b.normal) || !std::isfinite(planeTolerance) || planeTolerance < 0 ||
        a.instance != b.instance || Dot(a.normal,b.normal) < normalCos)
        return false;
    const auto d = Sub(a.position,b.position);
    return std::abs(Dot(d,a.normal)) <= planeTolerance &&
           std::abs(Dot(d,b.normal)) <= planeTolerance;
}
inline float PixelFootprint(float viewDepth, float tanHalfFovY,
                            float imageHeight, float tilePixels) {
    if (!(viewDepth > 0 && tanHalfFovY > 0 && imageHeight > 0 && tilePixels > 0))
        return 0;
    const float v = 2 * viewDepth * tanHalfFovY * tilePixels / imageHeight;
    return std::isfinite(v) ? v : 0;
}
inline uint32_t Cascade(float footprint, float baseSpacing, uint32_t levels) {
    if (levels == 0 || !(baseSpacing > 0) || !std::isfinite(footprint) ||
        !std::isfinite(baseSpacing))
        return 0;
    uint32_t level = 0;
    while (level < levels-1 && footprint >= baseSpacing*2) {
        baseSpacing *= 2;
        ++level;
    }
    return level;
}

// Stable world-instance identity, mesh-local leaf/brick voxel, normal side,
// and LOD. Never use the transient GPU instance or packed leaf array offset
// as a persistent identifier. Increment generation when geometry changes.
struct Key {
    uint64_t instance = 0;
    uint32_t generation = 0, leaf = 0, voxel = 0, side = 0, level = 0;
    bool operator==(const Key& b) const {
        return instance == b.instance && generation == b.generation &&
               leaf == b.leaf && voxel == b.voxel && side == b.side && level == b.level;
    }
};

struct Entry {
    Key key{};
    Vec3 irradiance{};
    uint64_t lastUse = 0;
    bool valid = false;
};

// Reference LRU (linear search intentionally; GPU uses bounded hash probes).
// One input per key per update after a deterministic reduction. History is
// clamped by the caller's lighting epoch: invalidate on a discontinuous change.
class Cache {
public:
    explicit Cache(size_t capacity) : entries_(capacity) {}
    size_t Capacity() const { return entries_.size(); }
    void Clear() { for (auto& e : entries_) e = {}; }

    std::optional<Vec3> Read(const Key& key, uint64_t frame) {
        for (auto& e : entries_) {
            if (e.valid && e.key == key) {
                e.lastUse = std::max(e.lastUse, frame);
                return e.irradiance;
            }
        }
        return std::nullopt;
    }

    bool Accumulate(const Key& key, Vec3 estimate, float hysteresis, uint64_t frame) {
        if (entries_.empty() || !Finite(estimate) || !std::isfinite(hysteresis) ||
            hysteresis < 0 || hysteresis >= 1)
            return false;
        for (auto& v : estimate) v = std::max(v, 0.0f);
        for (auto& e : entries_) {
            if (e.valid && e.key == key) {
                for (size_t i=0; i<3; ++i)
                    e.irradiance[i] = hysteresis*e.irradiance[i] + (1-hysteresis)*estimate[i];
                e.lastUse = std::max(e.lastUse, frame);
                return true;
            }
        }
        auto it = std::min_element(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
            if (a.valid != b.valid) return !a.valid;
            return a.lastUse < b.lastUse;
        });
        *it = {key, estimate, frame, true};
        return true;
    }
private:
    std::vector<Entry> entries_;
};

struct Budget {
    uint32_t maxProbes = 512, updates = 128, rays = 32, maxStepsPerRay = 24;
    float traceDistance = 12.0f, influenceRadius = 6.0f;
    bool Valid() const {
        return maxProbes > 0 && maxProbes <= 65536 && updates > 0 &&
               updates <= maxProbes && rays > 0 && rays <= 512 &&
               maxStepsPerRay > 0 && maxStepsPerRay <= 1024 &&
               std::isfinite(traceDistance) && traceDistance > 0 &&
               std::isfinite(influenceRadius) && influenceRadius > 0;
    }
    uint64_t RayCount() const { return Valid() ? uint64_t(updates)*rays : 0; }
    uint64_t StepCount() const { return RayCount()*maxStepsPerRay; }
};

// Fixed sun trajectory, normalized daylight coordinate in [0,1]. Bits
// represent samples INCLUDING both endpoints. Interpolate between them.
// Only valid for the baked trajectory and static occluders.
inline float SunVisibility(uint64_t mask, float daylight) {
    if (!std::isfinite(daylight)) return 0;
    const float p = std::clamp(daylight, 0.0f, 1.0f)*63.0f;
    const uint32_t a = static_cast<uint32_t>(p), b = std::min(a+1,63u);
    const float va = float((mask >> a)&1u), vb = float((mask >> b)&1u);
    return va+(vb-va)*(p-float(a));
}
} // namespace smvk::sdfgi
