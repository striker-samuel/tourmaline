// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// CPU reference for the four-class GPU PSO block-count / exclusive-prefix /
// scatter contract. Used for device-free correctness testing; the GPU path
// resolves geometry validity first and passes INVALID for rejected work.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace tour::gpu_scene {
inline constexpr uint32_t kPsoPrefixClasses = 4;
inline constexpr uint32_t kPsoPrefixGroup = 256;
inline constexpr uint32_t kPsoPrefixMaxWork = 1'048'576;
inline constexpr uint32_t kPsoPrefixInvalid = 0xffffffffu;

struct PsoPrefixPlan {
    using Counts = std::array<uint32_t, kPsoPrefixClasses>;
    Counts counts{};
    Counts offsets{};
    std::vector<Counts> blockCounts;
    std::vector<Counts> blockBases;
    // Absolute command indices by original work index; INVALID means no draw.
    std::vector<uint32_t> destinations;

    [[nodiscard]] static bool Build(std::span<const uint32_t> classes,
                                    Counts capacities,
                                    PsoPrefixPlan& destination) {
        if (classes.size() > kPsoPrefixMaxWork)
            return false;
        PsoPrefixPlan result;
        result.destinations.assign(classes.size(), kPsoPrefixInvalid);
        const size_t blocks = (classes.size() + kPsoPrefixGroup - 1) /
                              kPsoPrefixGroup;
        result.blockCounts.resize(blocks);
        result.blockBases.resize(blocks);
        for (size_t i = 0; i < classes.size(); ++i)
            if (classes[i] < kPsoPrefixClasses)
                ++result.blockCounts[i / kPsoPrefixGroup][classes[i]];

        for (size_t b = 0; b < blocks; ++b)
            for (uint32_t c = 0; c < kPsoPrefixClasses; ++c) {
                result.blockBases[b][c] = result.counts[c];
                result.counts[c] += result.blockCounts[b][c];
            }

        uint64_t offset = 0;
        for (uint32_t c = 0; c < kPsoPrefixClasses; ++c) {
            if (result.counts[c] > capacities[c] ||
                offset + capacities[c] > kPsoPrefixMaxWork)
                return false;
            result.offsets[c] = static_cast<uint32_t>(offset);
            offset += capacities[c];
        }

        for (size_t b = 0; b < blocks; ++b) {
            Counts localRanks{};
            const size_t end = ((b + 1) * kPsoPrefixGroup < classes.size())
                ? ((b + 1) * kPsoPrefixGroup) : classes.size();
            for (size_t i = b * kPsoPrefixGroup; i < end; ++i) {
                const uint32_t cls = classes[i];
                if (cls >= kPsoPrefixClasses)
                    continue;
                result.destinations[i] =
                    result.offsets[cls] + result.blockBases[b][cls] +
                    localRanks[cls]++;
            }
        }
        destination = std::move(result); // only commit after checks
        return true;
    }
};
} // namespace tour::gpu_scene
