// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Canonical Map streaming host adapter: no ProjectScenes/Cluster loader.
// Supply an immutable Scene/resource resolver, then register Provider() with
// WorldPartition. The opt-in historical implementation lives in
// LegacyMapSceneStreaming.h and must never be the default path.
#include <Engine/WorldPartition/DirectMapSceneStreaming.h>

namespace app {
using MapSceneStreaming = engine::worldpartition::DirectMapSceneStreaming;
} // namespace app
