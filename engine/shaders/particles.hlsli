// GPU particle system shared types (free-standing: no common.hlsli here so
// the compute stages compile without the scene PushConstants in scope).
// Must match the C++ mirrors in Engine/Rendergraph/particles.h (sizes are
// asserted there; this DXC rejects global static_assert).

#define MAX_PARTICLES 65536
#define MAX_PARTICLE_EMITTERS 16

// Spawn-volume shapes (ParticleEmitterGpu::life.w).
#define PARTICLE_SHAPE_CONE 0
#define PARTICLE_SHAPE_SPHERE 1
#define PARTICLE_SHAPE_POINT 2

// GPU particle (64 bytes). posLife.w <= 0 = dead slot (the draw VS collapses
// it to an off-screen degenerate; the sim pass recycles the slot).
// params: x = 1/lifeMax, y = emitter index, z = rotation speed (rad/s),
// w = spawn seed.
struct ParticleGpu
{
    float4 posLife;   // 0  xyz world pos, w life remaining (s)
    float4 velSize;   // 16 xyz velocity (m/s), w unused
    float4 colorRot;  // 32 rgb spawn tint, w rotation (rad)
    float4 params;    // 48 x invLifeMax, y emitterIdx, z rotSpeed, w seed
};

// GPU emitter (128 bytes). See the C++ header for the field-by-field doc;
// tail.x is the GPU-owned fractional spawn carry (lives in the separate
// emitterState buffer at runtime — kept here for layout symmetry only).
struct ParticleEmitterGpu
{
    float4 position;  // 0   xyz center, w spawn radius (m)
    float4 direction; // 16  xyz cone axis, w spread (rad)
    float4 color;     // 32  rgb base color, w emissive intensity
    float4 velocity;  // 48  x speedMin, y speedMax, z drag (1/s), w gravityScale
    float4 size;      // 64  x sizeStart, y sizeEnd (m), z rotSpeed (rad/s), w seed
    float4 life;      // 80  x lifeMin, y lifeMax (s), z rate (per s), w shape
    float4 fade;      // 96  x fadeIn, y fadeOut (life fractions), z colorJitter, w unused
    float4 tail;      // 112 x carry (GPU-owned), yzw pads
};

// BDA-addressed buffer views (typed pointers, forwardplus.cs.hlsl style).
struct ParticlesBuffer
{
    ParticleGpu particles[MAX_PARTICLES];
};

struct ParticleEmitterBuffer
{
    ParticleEmitterGpu emitters[MAX_PARTICLE_EMITTERS];
};

// GPU-owned fractional spawn carry, one float per emitter slot.
struct ParticleEmitterStateBuffer
{
    float carry[MAX_PARTICLE_EMITTERS];
};

// Free-list stack: slots[0] = live entry count, slots[1..count] = free pool
// slots. Emit pops from the top (atomicAdd count -1), sim pushes dead slots
// back (atomicAdd count +1).
struct ParticleFreeListBuffer
{
    uint count;
    uint slots[MAX_PARTICLES];
};

// ---- Push constants (one per stage family; sizes asserted on the C++ side).

// particles_emit.cs.hlsl (48 bytes).
struct ParticlesEmitPush
{
    vk::BufferPointer<ParticleEmitterBuffer> emitters;      // 0
    vk::BufferPointer<ParticleEmitterStateBuffer> emitterState; // 8
    vk::BufferPointer<ParticlesBuffer> particles;           // 16
    vk::BufferPointer<ParticleFreeListBuffer> freeList;     // 24
    uint emitterCount;                                      // 32
    uint maxParticles;                                      // 36
    float dt;                                               // 40
    uint frameSeed;                                         // 44
};

// particles_sim.cs.hlsl (48 bytes). Gravity arrives as scalars: a float3
// after three 8-byte pointers would straddle a 16-byte row and HLSL packs
// it away from the C++ offsets.
struct ParticlesSimPush
{
    vk::BufferPointer<ParticlesBuffer> particles; // 0
    vk::BufferPointer<ParticleFreeListBuffer> freeList; // 8
    vk::BufferPointer<ParticleEmitterBuffer> emitters;  // 16
    float gravityX;                               // 24
    float gravityY;                               // 28
    float gravityZ;                               // 32
    float dt;                                     // 36
    uint maxParticles;                            // 40
    uint pad0;                                    // 44
};

// particles_draw.vert/frag.hlsl (80 bytes). fpTileCountX == 0 tells the
// pixel shader the Forward+ tile lists are not fresh this frame (deferred
// shadows own the lights): it falls back to a capped brute-force loop.
// Only the draw pair defines PARTICLES_DRAW_TYPES (the emit/sim stages
// include this header without common.hlsli, whose types appear below).
#if defined(PARTICLES_DRAW_TYPES)
struct ParticlesDrawPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;         // 0
    vk::BufferPointer<ParticlesBuffer> particles;           // 8
    vk::BufferPointer<ParticleEmitterBuffer> emitters;      // 16
    vk::BufferPointer<ForwardPlusLightBuffer> fpLights;     // 24
    vk::BufferPointer<ForwardPlusTileCountBuffer> fpTileCounts;  // 32
    vk::BufferPointer<ForwardPlusTileIndexBuffer> fpTileIndices; // 40
    uint fpTileCountX;  // 48 0 = brute-force light fallback
    uint fpLightCount;  // 52
    uint fpTileSize;    // 56
    uint fpMaxPerTile;  // 60
    uint maxParticles;  // 64 VS instance guard
    uint pad0;          // 68
    uint pad1;          // 72
    uint pad2;          // 76
};
#endif // PARTICLES_DRAW_TYPES

// Compact PCG-style hash RNG (uint state in/out).
uint ParticleHash(uint v)
{
    v = v * 747796405u + 2891336453u;
    uint word = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (word >> 22u) ^ word;
}

float ParticleRand01(inout uint state)
{
    state = ParticleHash(state);
    return float(state) * (1.0 / 4294967296.0);
}

// Uniform point in the unit ball (sphere shape spawn).
float3 ParticleRandInBall(inout uint state)
{
    // Rejection is fine here (expected ~5.4 tries): the emit thread spawns
    // a handful of particles per frame, and a warped cube distribution
    // costs more lines than it saves.
    for (int i = 0; i < 16; ++i)
    {
        float3 p = float3(ParticleRand01(state), ParticleRand01(state),
            ParticleRand01(state)) * 2.0 - 1.0;
        if (dot(p, p) < 1.0)
            return p;
    }
    return float3(0.0, 0.0, 0.0);
}

// Uniform-ish unit vector (velocity directions).
float3 ParticleRandUnit(inout uint state)
{
    float3 p = ParticleRandInBall(state);
    float len = length(p);
    return len > 1e-5 ? p / len : float3(0.0, 1.0, 0.0);
}
