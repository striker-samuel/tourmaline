// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// LEVEL blob (PakAssetKind::Level): ONE composite pak blob per playable
// unit, packing the scene payload + its baked data blobs inline. The
// LEVEL — not the scene stem — owns the data; sub-blobs are addressed
// by their canonical keys (the vfs paths the standalone blobs would
// ride: "gi/cubemaps/<id>.cubemanifest", "gi/cubemaps/<id>_N.ktx2",
// "gi/<binding>.sggi", "scene"...), so existing loaders consume the
// unpacked bytes unchanged while the association lives one level up.
//
// Format (little-endian, magic-is-version, fail-closed — the pak
// discipline): NO FORMAT VERSIONS. A breaking change bumps the magic
// char (LVL1 -> LVL2); readers reject on mismatch, content recooks.
//
//   u32 magic "LVL1" (0x314C564C LE)
//   u32 nameLen + name bytes        the level's own ID (scene-independent,
//                                   e.g. "town" — the pak entry path is
//                                   levels/<name>.level)
//   u32 sceneKeyLen + key bytes     the sub-blob key of the scene payload
//                                   ("" = a data-only level)
//   u32 subCount
//   COLUMN BLOCKS (kind/key first, offset/size second — one-pass parse,
//   offset/size computed against the payload that follows):
//     subCount x { u32 kind, u32 keyLen + key bytes }
//     subCount x { u64 offset, u64 size }   (payload-relative)
//   payload: the concatenated sub-blob bytes, sub-blob order (raw; the
//   pak ENTRY may still LZ4 the whole level blob — sub-blobs share the
//   entry's fate)
//
// The reader borrows the caller's bytes (like PakReader over a pak
// image): payload spans point into the blob; nothing is copied.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Engine/Asset/pak.h>

namespace smvk {

inline constexpr uint32_t kLevelMagic = 0x314C564Cu; // "LVL1" LE

// One packed sub-blob record (parsed view; offset/size are
// payload-relative).
struct LevelSubBlob {
	PakAssetKind kind = PakAssetKind::Scene;
	std::string key;   // canonical (vfs-path-shaped) address
	uint64_t offset = 0;
	uint64_t size = 0;
};

// Parsed level view over borrowed bytes.
struct LevelBlob {
	std::string name;       // the level's own ID
	std::string sceneKey;   // "" = data-only level
	std::vector<LevelSubBlob> subs;
	std::span<const uint8_t> payload; // borrowed from the source blob

	// First sub-blob by exact key (nullptr = missing).
	const LevelSubBlob* Find(std::string_view key) const;
	// Sub-blob bytes by key (empty span = missing/empty).
	std::span<const uint8_t> Bytes(std::string_view key) const;
};

// Builder input (one packed payload; bytes copied into the blob).
struct LevelSubInput {
	PakAssetKind kind = PakAssetKind::Scene;
	std::string key;
	std::span<const uint8_t> bytes;
};

// Serializes the level blob (header + TOC + concatenated payloads).
// Rejects (returns false) on a missing key duplicate? — no: duplicates
// are allowed on the wire but Find returns the FIRST; builders should
// keep keys unique. Fails on nothing except allocation; empty bytes are
// legal (the key still occupies a slot).
bool BuildLevelBlob(std::string_view name, std::string_view sceneKey,
	std::span<const LevelSubInput> subs, std::vector<uint8_t>& out);

// Parses + validates (magic, truncation, sub-blob bounds). `blob` is
// borrowed: payload spans point into it. False on any malformation.
bool ParseLevelBlob(std::span<const uint8_t> blob, LevelBlob& out);

// Levels in a pak, TOC order (the Level-kind entries; the caller owns
// the reader/image lifetime — payloads borrow on demand via Payload()).
std::vector<const PakTocEntry*> ListPakLevels(const PakReader& reader);

} // namespace smvk
