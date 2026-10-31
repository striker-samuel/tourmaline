// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// The GAMEPLAY ENTITY-HANDLE template (EntityHandle<Tag>): typed 64-bit generational references
// to engine-owned runtime objects. Gameplay code COMPOSES these in its
// own objects (struct Knight { MeshEntityHandle mesh{};
// AnimatorHandle animator{}; AudioSourceHandle audio{}; }) — there is
// NO generic engine-side game-entity registry by design: the game owns
// gameplay logic + lifetimes; the World owns the ENGINE objects the
// handles reference (mesh instances, animators, audio sources).
//
// LAYOUT: high 32 bits = generation, low 32 bits = slot index. The
// all-zero value is "uninitialized / invalid" (default-initialized
// handles read false). The FIRST allocated generation is 1, so slot
// index 0 is usable; only the full 64-bit zero is the invalid value.
// Destroying the object bumps the slot's generation — every stale copy
// of the old handle fails validation forever (setting the C++ field
// back to {} does not invalidate OTHER copies; the generation does).
// A generation that would wrap retires the slot (never reused).
//
// STABILITY: handles survive scene REBUILDS (the World's resolve/
// fixup remaps internal NodeIds; the handle's slot->engine-object
// mapping is updated by the World, never invalidated by the rebuild).
// Handles are scoped to their owning World — cross-world use is
// rejected by validation (the slot won't resolve).
//
// Each subsystem validates its OWN handle type (the tag makes
// cross-subsystem misuse a compile error):
//   MeshEntityHandle  — World::SpawnMesh instances (a mesh INSTANCE,
//                       static or skinned — the asset key is MeshKey)
//   AnimatorHandle    — World::AddAnimator components
//   AudioSourceHandle — World::SpawnAudioSource emitters (authored
//                       .tour audio sources have no handle — they die
//                       with their scene; spawned ones die explicitly)
//
// vs the STABLE CATALOG KEYS (project.h's MeshKey/SkeletonKey/
// AnimationClipKey/SoundKey/...): a KEY names WHICH ASSET, a HANDLE
// names WHICH RUNTIME INSTANCE. Assets are immutable + shared;
// instances are World-owned + per-entity.

#include <cstdint>

namespace smvk {

template <typename Tag>
struct EntityHandle {
    uint64_t value = 0;

    [[nodiscard]] constexpr uint32_t Index() const {
        return static_cast<uint32_t>(value);
    }
    [[nodiscard]] constexpr uint32_t Generation() const {
        return static_cast<uint32_t>(value >> 32);
    }
    [[nodiscard]] constexpr explicit operator bool() const {
        return value != 0;
    }
    friend bool operator==(EntityHandle, EntityHandle) = default;
};

// Mint/validate helpers for the registries (the gameplay side never
// packs these by hand).
template <typename Tag>
[[nodiscard]] constexpr EntityHandle<Tag> MakeEntityHandle(uint32_t index,
    uint32_t generation) {
    return EntityHandle<Tag>{
        (static_cast<uint64_t>(generation) << 32) | index };
}

struct MeshEntityTag {};
struct AnimatorTag {};
struct AudioSourceTag {};

using MeshEntityHandle = EntityHandle<MeshEntityTag>;
using AnimatorHandle = EntityHandle<AnimatorTag>;
using AudioSourceHandle = EntityHandle<AudioSourceTag>;

} // namespace smvk
