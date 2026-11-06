// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Reusable world-space scalar producer for BC4_UNorm world-field bricks.
// A derived source supplies normalized values; this adapter owns the shared
// LOD lattice, texel-center/halo sampling, encoding and BC4 block layout.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <Engine/WorldFields/WorldField.h>
#include <Engine/WorldFields/WorldFieldCodec.h>

namespace engine::worldfields {

class ProceduralScalarBc4Source : public IWorldFieldBrickSource
{
public:
    // Normalized scalar in [0,1], BEFORE the field's Linear/SqrtUNorm encoding.
    // Implementations must be thread-safe (generation may run on workers).
    virtual float SampleNormalized(glm::vec3 worldPosition) const = 0;

    bool GenerateBrick(const WorldFieldDesc& field, uint32_t lod,
        int32_t x, int32_t y, int32_t z,
        std::byte* payload, size_t payloadBytes) final
    {
        const uint32_t stored = kBrickStoredRes;
        const size_t expected = StoredBrickBlockBytes(stored, 4, 4, 8);
        if (!payload || payloadBytes != expected ||
            field.format != smvk::Format::BC4_UNorm ||
            (field.encoding != WorldFieldEncoding::Linear &&
             field.encoding != WorldFieldEncoding::SqrtUNorm) ||
            lod >= field.lodCount || lod >= 32 ||
            !std::isfinite(field.baseVoxelSizeMeters) ||
            field.baseVoxelSizeMeters <= 0.0f)
            return false;

        const float voxel = std::ldexp(field.baseVoxelSizeMeters, int(lod));
        const glm::vec3 brickMin = field.originMeters +
            glm::vec3(float(x), float(y), float(z)) *
                (float(kBrickInteriorRes) * voxel);

        std::vector<uint8_t> texels(size_t(stored) * stored * stored);
        size_t i = 0;
        for (uint32_t tz = 0; tz < stored; ++tz)
            for (uint32_t ty = 0; ty < stored; ++ty)
                for (uint32_t tx = 0; tx < stored; ++tx) {
                    // The halo samples the same world lattice as adjacent
                    // bricks; +0.5 aligns it with hardware trilinear filtering.
                    const glm::vec3 p = brickMin + glm::vec3(
                        float(tx) + 0.5f - float(kBrickBorder),
                        float(ty) + 0.5f - float(kBrickBorder),
                        float(tz) + 0.5f - float(kBrickBorder)) * voxel;
                    const float sampled = SampleNormalized(p);
                    const float v = std::isfinite(sampled)
                        ? std::clamp(sampled, 0.0f, 1.0f) : 0.0f;
                    const float encoded = field.encoding == WorldFieldEncoding::SqrtUNorm
                        ? std::sqrt(v) : v;
                    texels[i++] = uint8_t(encoded * 255.0f + 0.5f);
                }
        return Bc4EncodeBrick(texels.data(), stored, payload, payloadBytes) == expected;
    }
};

} // namespace engine::worldfields
