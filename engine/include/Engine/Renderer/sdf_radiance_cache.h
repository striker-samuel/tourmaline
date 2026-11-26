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

// Independent payload/visibility options; not a renderer setting until the
// GPU path is wired. Existing full irradiance path remains the default.
enum class LightingMode : uint8_t { FullIrradiance, TintOnly };
struct LightingOptions {
    LightingMode mode = LightingMode::FullIrradiance;
    bool interiorDarkening = false;
    bool sunWeightedTint = false;
};
using Chroma = std::array<float, 2>;

// Linear RGB input. Keep absolute chroma (dark materials bleed less).
// Neutral colors contribute zero; no hue wrap during filtering.
inline Chroma MaterialChroma(Vec3 albedo) {
    if (!Finite(albedo)) return {};
    for (float& v : albedo) v = std::clamp(v,0.0f,1.0f);
    return {0.5f*(albedo[0]-albedo[2]),
            0.5f*albedo[1]-0.25f*(albedo[0]+albedo[2])};
}
inline Chroma BlendChroma(Chroma oldValue, Chroma estimate, float hysteresis) {
    if (!std::isfinite(hysteresis)) hysteresis = 0;
    hysteresis = std::clamp(hysteresis,0.0f,1.0f);
    for (size_t i=0; i<2; ++i) {
        if (!std::isfinite(oldValue[i])) oldValue[i] = 0;
        if (!std::isfinite(estimate[i])) estimate[i] = 0;
        oldValue[i] = std::clamp(hysteresis*oldValue[i] +
                                  (1-hysteresis)*estimate[i],-0.5f,0.5f);
    }
    return oldValue;
}
inline uint8_t EncodeChroma(float v) {
    if (!std::isfinite(v)) v = 0;
    const int q = int(std::lround(std::clamp(v,-0.5f,0.5f)*254.0f));
    return uint8_t(q < 0 ? q+256 : q);
}
inline float DecodeChroma(uint8_t v) {
    const int q = v < 128 ? int(v) : int(v)-256;
    return float(std::max(q,-127))/254.0f;
}
inline uint8_t EncodeSky(float v) {
    if (!std::isfinite(v)) v = 1; // neutral for unavailable data
    return uint8_t(std::lround(std::clamp(v,0.0f,1.0f)*255.0f));
}
struct TintPayload {
    uint8_t co = 0, cg = 0, sky = 255;
};
static_assert(sizeof(TintPayload)==3);
inline TintPayload PackTint(Chroma tint, float sky) {
    return {EncodeChroma(tint[0]),EncodeChroma(tint[1]),EncodeSky(sky)};
}
inline Chroma UnpackTint(TintPayload p) {
    return {DecodeChroma(p.co),DecodeChroma(p.cg)};
}

// Sum all scheduled cosine-weighted hemisphere samples, not only hits.
// Misses contribute zero; incomplete traces must be rejected by the caller.
// Optional sun visibility scales the hit contribution, not the receiver.
// skyWeight keeps shadowed materials from losing all color under skylight.
struct TintEstimator {
    Chroma sum{};
    uint32_t samples = 0;
    void AddMiss() { ++samples; }
    void AddHit(Vec3 albedo, float distanceWeight, float hitSunVisibility = 1,
                float skyWeight = 1) {
        ++samples;
        if (!std::isfinite(distanceWeight) || !std::isfinite(hitSunVisibility) ||
            !std::isfinite(skyWeight)) return;
        const auto c = MaterialChroma(albedo);
        const float w = std::clamp(distanceWeight,0.0f,1.0f) *
            (std::clamp(skyWeight,0.0f,1.0f) +
             (1-std::clamp(skyWeight,0.0f,1.0f))*std::clamp(hitSunVisibility,0.0f,1.0f));
        for (size_t i=0;i<2;++i) sum[i] += c[i]*w;
    }
    Chroma Estimate() const {
        if (!samples) return {};
        return {sum[0]/float(samples),sum[1]/float(samples)};
    }
};

// Weighted visibility to the actual sky, normalized to an unoccluded
// hemisphere with the SAME directional weights. Unknown is not a sky miss.
// Return invalid for incomplete coverage; caller chooses cached/baked fallback.
struct SkyEstimator {
    float visible = 0, total = 0;
    bool complete = true;
    void Add(float weight, bool escaped, bool known = true) {
        if (!known || !std::isfinite(weight) || weight < 0) {
            complete = false;
            return;
        }
        total += weight;
        if (escaped) visible += weight;
    }
    std::optional<float> Estimate() const {
        if (!complete || !(total > 0) || !std::isfinite(total)) return std::nullopt;
        return std::clamp(visible/total,0.0f,1.0f);
    }
};

// Optional large-scale interior term for the sky ambient slice only.
// AO remains an independent local term. A nonzero floor is artistic,
// not physical; local lights and emissive must not be darkened by this.
inline float InteriorSkyScale(float visibility, float strength, float floor) {
    if (!std::isfinite(visibility) || !std::isfinite(strength) || !std::isfinite(floor))
        return 1;
    visibility = std::clamp(visibility,0.0f,1.0f);
    strength = std::clamp(strength,0.0f,1.0f);
    floor = std::clamp(floor,0.0f,1.0f);
    return 1-strength*(1-floor)*(1-visibility);
}

// Apply tint to existing linear diffuse ambient (before receiver albedo).
// Normalize to the original Rec.709 luminance; this intentionally cannot
// add energy or illuminate a black input. Interior scale is applied apart.
inline Vec3 ApplyTint(Vec3 ambient, Chroma chroma, float strength) {
    if (!Finite(ambient)) return {};
    for (auto& v:ambient) v=std::max(v,0.0f);
    if (!std::isfinite(strength) || !std::isfinite(chroma[0]) ||
        !std::isfinite(chroma[1])) return ambient;
    strength=std::clamp(strength,0.0f,1.0f);
    const float co=std::clamp(chroma[0],-0.5f,0.5f)*strength;
    const float cg=std::clamp(chroma[1],-0.5f,0.5f)*strength;
    Vec3 tinted{ambient[0]*std::max(0.0f,1+co-cg),
                ambient[1]*std::max(0.0f,1+cg),
                ambient[2]*std::max(0.0f,1-co-cg)};
    constexpr Vec3 luma{0.2126f,0.7152f,0.0722f};
    const float before=Dot(ambient,luma), after=Dot(tinted,luma);
    if (!(after>1e-8f) || !std::isfinite(after) || !std::isfinite(before))
        return ambient;
    for(auto& v:tinted) v*=before/after;
    return tinted;
}
} // namespace smvk::sdfgi
