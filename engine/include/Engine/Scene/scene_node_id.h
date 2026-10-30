// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// The scene node identity vocabulary — shared by scene.h (the graph)
// and scene_tables.h (the typed association tables) without a cycle.
// NodeId stays a dense array index for the table migration epochs
// (3–9); stable slots + generations arrive with the world-authority
// epochs (6b/6c, docs/scene_tables_migration.md) and change ONLY this
// definition's backing representation, never the row contracts.
#include <cstdint>

using NodeId = uint32_t;
