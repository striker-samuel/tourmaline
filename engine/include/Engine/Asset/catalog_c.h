// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Stable, C-compatible project-catalog ABI. C++ and C bindings are emitted
// from the SAME ProjectCatalog model, with the SAME identity fingerprint.
// No C++ standard-library types or compiler-specific object layouts here.

#include <stddef.h>
#include <stdint.h>

#define TOUR_C_CATALOG_ABI_VERSION 1u

// Kind values are the deployed appmanifest/pak identity ABI. Do not renumber.
enum tour_c_asset_kind {
    TOUR_C_KIND_NIL = 0,
    TOUR_C_KIND_SCENE = 1,
    TOUR_C_KIND_TEXTURE = 2,
    TOUR_C_KIND_MESH = 3,
    TOUR_C_KIND_SOUND = 4,
    TOUR_C_KIND_SHADER = 5,
    TOUR_C_KIND_GI = 6,
    TOUR_C_KIND_AUDIOGRAPH = 7,
    TOUR_C_KIND_SPRITESET = 8,
    TOUR_C_KIND_STATEMACHINE = 9,
    TOUR_C_KIND_NAVMESH = 10
};

// NUL-terminated strings are borrowed from the generated project_c.h.
// Owning pak names/filenames are SELECTION data, not part of asset identity.
typedef struct tour_c_asset_desc {
    uint64_t id;
    uint8_t kind;
    const char* path;
    const char* scene_name; // reserved, currently empty
} tour_c_asset_desc;

typedef struct tour_c_package {
    const char* name;
    const char* file;
} tour_c_package;

typedef struct tour_c_audio_cue {
    uint64_t graph;
    uint32_t ordinal;
} tour_c_audio_cue;

typedef struct tour_c_catalog {
    uint32_t abi_version;
    const tour_c_asset_desc* assets;
    size_t asset_count;
    uint64_t fingerprint;
    uint64_t entry_scene;
} tour_c_catalog;
