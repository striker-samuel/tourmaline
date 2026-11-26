// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Optional OpenUSD adapter load boundary. `Import.exe` links the USD-free
// `source_import` and dynamically loads `usd_import.<ext>` only when a
// `.usd` / `.usda` / `.usdc` / `.usdz` source is detected, so a process that
// only imports glTF/OBJ never pulls the OpenUSD runtime (and never dies with
// "DLL not found" when the USD SDK is not on PATH). The function-pointer
// table is POD so the C-ABI export stays clean; the pointed-to signatures use
// the same tinygltf/path ABI as the in-process import (one toolchain, one
// CRT, so no STL object crosses a mismatched boundary).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tinygltf {
class Model;
}
namespace tour {
namespace placement {
struct BakedPlacementAsset;
}
} // namespace tour

namespace source_import {

struct UsdAdapterApi {
    // The .usd stage -> in-memory tinygltf::Model adapter. Same signature as
    // the in-process `LoadUsdFileAsModel`.
    bool (*loadUsdFileAsModel)(tinygltf::Model&,
                               const std::filesystem::path&,
                               std::vector<std::string>&) = nullptr;
    // The baked placement asset (PointInstancer expansion) the last .usd
    // import produced; null when the adapter is absent / no instancer.
    const tour::placement::BakedPlacementAsset* (*lastBakedPlacement)() = nullptr;
    tour::placement::BakedPlacementAsset* (*mutableBakedPlacement)() = nullptr;
};

// Dynamically loads the adapter DLL (`usd_import.dll` /
// `libusd_import.so`) from beside the running executable and fills `api`.
// Returns false (with `*error` set) when the DLL is absent — "USD import
// disabled" — without touching the OpenUSD runtime. Safe to call repeatedly.
bool LoadUsdAdapter(UsdAdapterApi* api, std::string* error);

} // namespace source_import
