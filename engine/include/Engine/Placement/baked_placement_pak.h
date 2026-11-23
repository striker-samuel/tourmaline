// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Baked-placement pak loading: reads the cooked <stem>.baked_placement_cooked
// blob the pak embeds (PakAssetKind::BakedPlacement) for a .geometry family.
// The world loader uses this to hydrate the World's BakedPlacementRef set; the
// renderer then auto-loads the runtimes. No app-side placement wiring.

#include <string>

#include <Engine/Asset/pak.h>
#include <Engine/Placement/baked_placement_runtime.h>

namespace tour::placement {

// "<stem>.geometry" -> "<stem>.baked_placement_cooked" (the pak blob path the
// cook's BakedPlacementKind embed used).
[[nodiscard]] inline std::string BakedPlacementSidecarPath(const std::string& familyPath) {
    if (familyPath.size() >= 9 && familyPath.compare(familyPath.size() - 9, 9, ".geometry") == 0)
        return familyPath.substr(0, familyPath.size() - 9) + ".baked_placement_cooked";
    return familyPath + ".baked_placement_cooked";
}

// Reads + deserializes the cooked baked placement blob for a .geometry family.
// Returns false when the pak carries no baked placement for that family (the
// scene had no PointInstancer) or the blob is malformed.
[[nodiscard]] inline bool LoadBakedPlacementFromPak(const smvk::PakReader& reader,
                                                    const std::string& familyPath,
                                                    BakedPlacementCooked& out) {
    const auto* entry =
        reader.Find(smvk::PakAssetKind::BakedPlacement, BakedPlacementSidecarPath(familyPath));
    if (!entry)
        return false;
    try {
        const auto bytes = reader.Payload(*entry);
        return DeserializeBakedPlacementCooked(bytes.data(), bytes.size(), out);
    } catch (...) {
        return false;
    }
}

} // namespace tour::placement

namespace smvk::placement {
using ::tour::placement::BakedPlacementSidecarPath;
using ::tour::placement::LoadBakedPlacementFromPak;
} // namespace smvk::placement
