// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// meshparticles.hlsli — the GPU MESH-particle mode (PR06): spinning 3D
// mesh variants on the existing particle machinery's pattern (the GPU
// pool + the free-list stack + the emitter carry — particles.hlsli's
// billboard form stays untouched and correct). The binning/indirect
// path reuses the PR03 chain's SHAPES (placement_cull.hlsli's bins/
// mask/visible/indirect buffers — the approach sharing the plan calls
// for; the yaw-only 40-byte placement RenderInstance is deliberately
// NOT imposed on spinning particles — a full quaternion carries them).
//
// Bins are MESH variants (meshId 0..MESHPARTICLE_MAX_BINS-1): alive
// particles bin per mesh, the chain builds the visible-index list + the
// indirect commands, and the draw VS fetches
// visible[SV_InstanceID] -> particle slot -> the quaternion model
// matrix. DXC-2018 compatible; 32-bit integer math only.

#ifndef MESHPARTICLES_HLSLI
#define MESHPARTICLES_HLSLI

#include "particles.hlsli"
#include "placement_cull.hlsli"

#define MESHPARTICLE_MAX 65536
#define MESHPARTICLE_MAX_BINS 8
#define MESHPARTICLE_MAX_EMITTERS 16

// Must match C++ MeshParticleGpu (96 bytes, 16-aligned). posLife.w <= 0
// marks the slot dead (the sim pushes it back onto the free-list; the
// cull mask keeps it out of the bins, so the draw never fetches it).
// params: x = 1/lifeMax, y = meshId, z = sizeStart (m), w = sizeEnd (m)
// — the envelope's endpoints persist so the sim's per-frame size is the
// clean lerp(sizeStart, sizeEnd, ageFraction), never a drifting value.
struct MeshParticleGpu
{
    float4 posLife;     // 0   xyz world pos, w life remaining (s)
    float4 velSize;     // 16  xyz velocity (m/s), w CURRENT uniform size (m)
    float4 rot;         // 32  orientation quaternion (xyzw, normalized)
    float4 angVelGrav;  // 48  xyz angular velocity (axis * rad/s), w gravityScale
    float4 colorEmit;   // 64  rgb tint, w emissive intensity
    float4 params;      // 80  x invLifeMax, y meshId, z sizeStart, w sizeEnd
};

struct MeshParticleBuffer { MeshParticleGpu p[MESHPARTICLE_MAX]; };

// Must match C++ MeshParticleEmitterGpu (128 bytes, 16-aligned).
// The spawn shapes + the RNG reuse the billboard kernel's forms.
struct MeshParticleEmitterGpu
{
    float4 position;   // 0   xyz center, w spawn radius (m)
    float4 direction;  // 16  xyz cone axis (normalized), w spread (rad)
    float4 velocity;   // 32  x speedMin, y speedMax, z drag (1/s), w gravityScale
    float4 life;       // 48  x lifeMin, y lifeMax, z rate (1/s), w meshId
    float4 spin;       // 64  x angVelMin, y angVelMax (rad/s), z seed, w scaleJitter
    float4 size;       // 80  x sizeStart, y sizeEnd, z tintJitter, w emissive
    float4 color;      // 96  rgb base tint, w unused
    float4 tail;       // 112 carry slot (GPU-owned via emitterState; rows upload 0)
};

struct MeshParticleEmitterBuffer {
    MeshParticleEmitterGpu e[MESHPARTICLE_MAX_EMITTERS];
};

// Must match C++ MeshParticlesEmitPush (48 bytes). The free-list +
// carry views are the BILLBOARD forms (particles.hlsli) — the same
// machinery, a different pool.
struct MeshParticlesEmitPush
{
    vk::BufferPointer<MeshParticleEmitterBuffer> emitters;   // 0
    vk::BufferPointer<ParticleEmitterStateBuffer> emitterState; // 8
    vk::BufferPointer<MeshParticleBuffer> particles;         // 16
    vk::BufferPointer<ParticleFreeListBuffer> freeList;     // 24
    uint emitterCount;  // 32 live emitters this frame
    uint maxParticles;  // 36 pool bound
    float dt;           // 40
    uint frameSeed;     // 44 RNG stream salt (monotonic frame)
};

// Must match C++ MeshParticlesSimPush (48 bytes).
struct MeshParticlesSimPush
{
    vk::BufferPointer<MeshParticleBuffer> particles;         // 0
    vk::BufferPointer<ParticleFreeListBuffer> freeList;     // 8
    float gravityX;     // 16 global gravity (m/s^2)
    float gravityY;     // 20
    float gravityZ;     // 24
    float dt;           // 28
    uint maxParticles;  // 32 pool bound
    uint frameSeed;     // 36 (the spin integration needs no RNG)
    uint pad0;          // 40
    uint pad1;          // 44
};

// Must match C++ MeshParticlesCullPush (208 bytes, dx-layout). The
// sphere test reuses PlacementSphereVisible (placement_cull.hlsli) —
// the per-MESH bounds are the same conservative-sphere form. The kernel
// walks the WHOLE pool (maxParticles — the CPU-known dispatch bound):
// live slots sit anywhere in the pool (the free-list is a stack), dead
// slots early-out on posLife.w <= 0.
struct MeshParticlesCullPush
{
    vk::BufferPointer<MeshParticleBuffer> particles;        // 0
    vk::BufferPointer<PlacementMaskBuffer> mask;          // 8
    vk::BufferPointer<PlacementBinsBuffer> bins;          // 16
    vk::BufferPointer<RenderViewBuffer> renderViews;       // 24
    uint renderViewIndex; // 32
    uint cullFlags;       // 36 (the PLACEMENT_CULL_* bits)
    uint binCount;        // 40 <= MESHPARTICLE_MAX_BINS
    uint maxParticles;    // 44 the pool bound (the dispatch width)
    uint pad0;            // 48
    uint pad1;            // 52
    uint pad2;            // 56
    uint pad3;            // 60
    float4 meshBounds[MESHPARTICLE_MAX_BINS]; // 64..192 center.xyz, radius.w
    float3 camPos;        // 192 the species-distance origin (unused v1)
    float padE;           // 204
};

// Must match C++ MeshParticlesScatterPush (48 bytes).
struct MeshParticlesScatterPush
{
    vk::BufferPointer<MeshParticleBuffer> particles;        // 0
    vk::BufferPointer<PlacementMaskBuffer> mask;          // 8
    vk::BufferPointer<PlacementBinsBuffer> bins;          // 16
    vk::BufferPointer<PlacementVisibleBuffer> visible;     // 24
    uint binCount;      // 32
    uint maxParticles;  // 36 the pool bound (the dispatch width)
    uint pad0;          // 40
    uint pad1;          // 44
};

#endif
