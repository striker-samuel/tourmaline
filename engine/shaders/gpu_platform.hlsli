// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPU resource-declaration vocabulary — the DECLARATION layer only.
//
// GPU_PTR / GPU_PUSH name the constructs the engine's shaders already
// use so declaration sites stop repeating backend syntax, and future
// backend work (a different address model, a different push block
// spelling) has exactly ONE file to touch. Every macro expands to the
// exact declaration DXC emits today: no wrapper types, no extra
// indirection, no runtime change — the migration was verified
// byte-identical SPIR-V across the whole module set.
//
//   GPU_PTR(T)          a GPU-addressable buffer reference (BDA)
//   GPU_PTR_READ(T)     ... this shader only READS through it
//   GPU_PTR_WRITE(T)    ... this shader WRITES through it
//   GPU_PUSH(T, name)   the entry point's push-constant block
//
// GPU_PTR_READ / GPU_PTR_WRITE currently expand to plain GPU_PTR: the
// distinction DOCUMENTS intent at the declaration site, it does not
// enforce access (the BDA model has no read-only pointer). Use plain
// GPU_PTR when access is genuinely mixed — read-modify-write counters,
// in-place scratch, shared multi-phase push structs whose members
// change direction per dispatch (see DsPhasePush). A label that lies is
// worse than no label: keep these truthful.
//
// Dereferencing is UNCHANGED and stays that way: pcF.draws.Get().draws[i].
// The declaration layer and the access layer are separate concerns; a
// GPU_LOAD/GPU_STORE-style access wrapper can be introduced later,
// independently, only if a concrete need shows up.

#define GPU_PTR(T) vk::BufferPointer<T>

#define GPU_PTR_READ(T)  GPU_PTR(T)
#define GPU_PTR_WRITE(T) GPU_PTR(T)

#define GPU_PUSH(T, name) \
    [[vk::push_constant]] ConstantBuffer<T> name
