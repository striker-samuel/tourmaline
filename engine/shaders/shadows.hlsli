#pragma once


#ifndef NUM_CASCADES
#define NUM_CASCADES 4
#endif

#define Epsilon 1
#include "common.hlsli"

struct CascadeMeta
{
    float4x4 lightVP;
    float splitEnd; // view-space depth end for this cascade
    float bias;
    float normalBias;

    float2 invShadowSize; // 1/(atlasWidth, atlasHeight) OR 1/(tileWidth,tileHeight) depending how you do PCF
    float2 uvScale; // tile scale in atlas UV
    float2 uvBias; // tile offset in atlas UV
    float2 uvClampMin; // clamp inside tile (avoid bleeding)
    float2 uvClampMax;
};

int SelectCascade(float viewDepth, CascadeMeta cascades[NUM_CASCADES])
{
	[unroll]
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        if (viewDepth < cascades[i].splitEnd)
            return i;
    }
    return NUM_CASCADES - 1;
}


// BDA discipline (profiler follow-up): the old signature took the whole
// cascade array BY VALUE (GpuCascadeDescription cascades[4] = 512 B) — a
// dynamically indexed by-value array must be materialized into
// addressable/scratch storage (the "Local Memory Load/Store" stalls the
// lit pass profiled), on top of the physical-storage reads that fill it.
// Selecting through the BDA pointer instead reads ONE splitEnd scalar
// per candidate cascade (narrow computed-offset physical-storage loads,
// CSE'd by the compiler) and never builds an aggregate; the caller then
// loads only the actually-selected 128-byte cascade. Same traversal
// semantics: first cascade whose splitEnd covers viewDepth, last one as
// the fallback.
int SelectCascadeGpu(
    float viewDepth,
    vk::BufferPointer<DirectionalCascadeBuffer> cb,
    uint numCascades)
{
    int selected = int(numCascades) - 1;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        if ((uint) i >= numCascades)
            break;

        float split =
            cb.Get().dir.cascades[i].splitEnd;

        if (viewDepth <= split)
        {
            selected = i;
            break;
        }
    }

    return selected;
}

float PCFShadow(
    uint samplerId,
	float2 uv,
	float currentDepth, // receiver depth in shadow map depth space (0..1)
	uint shadowIdx
)
{
    uint w, h;
    textures[shadowIdx].GetDimensions(w, h);
    float2 texelSize = 1.0f / float2(w, h);

	// Kernel radius in texels:
	// 0 => 1x1
	// 1 => 3x3
	// 2 => 5x5
	// 3 => 7x7
    const int radius = 1; // <-- set what you actually want

    float sum = 0.0f;
    int samples = 0;

	[unroll]
    for (int y = -radius; y <= radius; ++y)
    {
			[unroll]
        for (int x = -radius; x <= radius; ++x)
        {
            float2 offset = float2(x, y) * texelSize;

            float d = textures[shadowIdx]
            .SampleCmpLevelZero(
                comparisonSamplers[samplerId],
                uv + offset,
                currentDepth
            );

            sum += (currentDepth <= d) ? 1.0f : 0.0f;
            samples++;
        }
    }

    return sum / samples;
}

float PCFShadowAtlas(
    uint samplerId,
    float2 uv,
    float currentDepth,
    uint shadowIdx,
    float2 texelSize,
    float2 uvClampMin,
    float2 uvClampMax)
{
    float sum = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 sampleUV = clamp(
                uv + float2(x, y) * texelSize,
                uvClampMin,
                uvClampMax);

            float d = textures[shadowIdx]
                .SampleLevel(samplers[samplerId], sampleUV, 0).r;

            sum += currentDepth <= d;
        }
    }
    

    return sum * (1.0f / 9.0f);
}

// Sec. 4.2 oriented depth bias (slope-scaled, per-cascade NDC).
// Shadowmaps render with no HW bias. baseBiasNDC is the per-cascade NDC
// bias baked on the CPU as worldBias / lightDepthRange (see
// BuildDirectionalLight), so the world-space effect is uniform across
// cascades instead of growing with the far ortho range. The NDotL slope
// term gives grazing surfaces (~the far-wall acne case) more bias and
// facing surfaces just a small constant – continuous, no +/-k step, so
// no terminator line streak. lightDirection is the travel direction
// (i.e. -L). isSSS parked for a future SSS/transmission path.
float GetOrientedBias(float3 faceNormal,
	float3 lightDirection,
	float baseBiasNDC,
	bool isSSS // parked: always false for now, kept just in case
)
{
    float nDotL = saturate(dot(faceNormal, -lightDirection));
    float slope = 1.0f - nDotL; // 0 facing, 1 grazing/back
    float facing = isSSS ? 1.0f : 1.0f;
    // 30% constant (residual acne) + 70% slope (grazing acne).
    return -baseBiasNDC * (0.3f + 0.7f * slope) * facing;
}

// Samples one cascade tile; valid=false when the (normal-biased) receiver
// falls outside the cascade light frustum so the caller can fall through
// to the next cascade instead of popping to unshadowed.
// BDA discipline (profiler follow-up): takes the cascade buffer POINTER +
// cascade index, not a by-value GpuCascadeDescription — a 128-byte
// by-value parameter per call (dynamically indexed from the caller's
// loop) made the compiler stage aggregate copies in scratch memory
// ("Local Memory" stalls). Each field read below is a narrow
// computed-offset physical-storage load (the address math is CSE'd), so
// only the fields actually used cross the BDA boundary and no aggregate
// is ever built.
float SampleCascadeTile(
    vk::BufferPointer<DirectionalCascadeBuffer> cb,
    uint cascadeIndex,
    float3 worldPos,
    float3 faceNormal,
    float3 lightDirection,
    uint samplerId,
    uint idx,
    out bool valid
)
{
    valid = false;
    // Receiver-side normal bias (world units, from c.normalBias): offsets
    // the sample point along the geometric normal before projection so
    // thin/colliding trim doesn't leak between shadow texels. Unlike a
    // depth-only bias it doesn't detach contact shadows along the light
    // axis, and unlike +/-k it has no terminator discontinuity.
    float3 biasedWorld =
        worldPos +
        faceNormal * cb.Get().dir.cascades[cascadeIndex].normalBias;

    float4 clip =
        mul(
            cb.Get().dir.cascades[cascadeIndex].lightVP,
            float4(biasedWorld, 1.0f)
        );

    float3 ndc =
        clip.xyz / clip.w;

    float2 uv =
        ndc.xy * 0.5f + 0.5f;

    if (
        ndc.z <= 0.0f ||
        ndc.z >= 1.0f ||
        uv.x < 0.0f ||
        uv.x > 1.0f ||
        uv.y < 0.0f ||
        uv.y > 1.0f
    )
    {
        return 1.0f;
    }

    float2 uvAtlas =
        uv * cb.Get().dir.cascades[cascadeIndex].uvScale +
        cb.Get().dir.cascades[cascadeIndex].uvBias;

    // Sec. 4.2: slope-scaled NDC bias; c.bias is the per-cascade
    // worldBias/depthRange baked on the CPU so far cascades don't
    // over-bias (peter-panning) while near ones under-bias (acne).
    float receiverDepth =
        ndc.z +
        GetOrientedBias(
            faceNormal,
            lightDirection,
            cb.Get().dir.cascades[cascadeIndex].bias,
            false);
        // parked SSS variant: ndc.z + GetOrientedBias(faceNormal, lightDirection, c.bias, true);

    valid = true;
    return PCFShadowAtlas(
    samplerId,
    uvAtlas,
    receiverDepth,
    idx,
    cb.Get().dir.cascades[cascadeIndex].invShadowSize,
    cb.Get().dir.cascades[cascadeIndex].uvClampMin,
    cb.Get().dir.cascades[cascadeIndex].uvClampMax);
}

// BDA discipline (profiler follow-up): this used to take FrameUniforms
// (1.3 KB+) and GpuDirectionalLightDescription (624 B with the 512 B
// embedded cascade array) BY VALUE — one huge copy per shaded fragment,
// and the dynamic dir.cascades[k] indexing forced the copies into
// scratch/private storage (the "Local Memory Load/Store" stalls paired
// with the global BDA read stalls in the profile). New contract:
//   - lightDirection/shadowMapIndex come from the Light the caller
//     already holds (the cascade buffer's copies of those were seeded
//     from the same values on upload, see BuildDirectionalLight),
//   - numCascades from pc (kept in lockstep by the app: NUM_CASCADES),
//   - view: the ONE FrameUniforms member this path reads (world->view
//     depth for cascade selection),
//   - cb: the cascade buffer POINTER — callees (SelectCascadeGpu /
//     SampleCascadeTile) read only the fields they need as narrow
//     physical-storage loads and no aggregate is ever materialized.
float GetShadowFactorCascaded(
    uint samplerId,
    float3 worldPos,
    float3 faceNormal,
    float3 lightDirection,
    int shadowMapIndex,
    uint numCascades,
    float4x4 view,
    vk::BufferPointer<DirectionalCascadeBuffer> cb
)
{
    if (
        shadowMapIndex < 0 ||
        numCascades == 0u
    )
    {
        // Legacy single-map fallback is parked (see GetShadowFactor above).
        return 1.0f;
    }

    float3 viewPos =
        mul(
            view,
            float4(worldPos, 1.0f)
        ).xyz;

    float viewDepth =
        max(-viewPos.z, 0.0f);

    int firstCascade =
        SelectCascadeGpu(
            viewDepth,
            cb,
            numCascades
        );

    uint idx =
        NonUniformResourceIndex(
            (uint) shadowMapIndex
        );

    // First valid cascade at/after the view-depth selection. Falling
    // through (instead of returning lit when the selected tile misses)
    // removes the hard pop from "fully shadowed" to "barely shadowed"
    // when a receiver sits outside the selected cascade's light bounds.
    int usedK = -1;
    float shadow = 1.0f;
    for (int k = firstCascade; k < int(numCascades); ++k)
    {
        bool valid = false;
        float s = SampleCascadeTile(
            cb,
            (uint) k,
            worldPos,
            faceNormal,
            lightDirection,
            samplerId,
            idx,
            valid);
        if (valid)
        {
            usedK = k;
            shadow = s;
            break;
        }
    }
    if (usedK < 0)
        return 1.0f;

    // Border blend into the next cascade over the last 10% of this
    // cascade's view-depth band. The next tile has fatter texels and a
    // larger baked bias (tile detail washes out there), so a hard switch
    // reads as a shadow-density step; lerping hides the seam. splitEnd
    // reads stay scalar (BDA discipline: no cascade aggregate copies).
    if (usedK + 1 < int(numCascades))
    {
        float splitEndK =
            cb.Get().dir.cascades[usedK].splitEnd;
        float splitStart =
            (usedK == 0)
                ? 0.0f
                : cb.Get().dir.cascades[usedK - 1].splitEnd;
        float bandW = (splitEndK - splitStart) * 0.1f;
        if (bandW > 1e-4f && viewDepth > splitEndK - bandW)
        {
            float t = smoothstep(splitEndK - bandW, splitEndK, viewDepth);
            bool nextValid = false;
            float nextShadow = SampleCascadeTile(
                cb,
                (uint) (usedK + 1),
                worldPos,
                faceNormal,
                lightDirection,
                samplerId,
                idx,
                nextValid);
            if (nextValid)
                shadow = lerp(shadow, nextShadow, t);
        }
    }
    return shadow;
}
//// ---- Cascaded path – atlas sampling (upgraded builder) ----
//float GetShadowFactorCascaded(
//    uint comparisonSamplerId,
//    float3 worldPos,
//    Light light,
//    FrameUniforms frame,
//    GpuDirectionalLightDescription dir
//)
//{
//    if (light.shadow_map_index < 0 || dir.numCascades <= 0)
//        return GetShadowFactor(comparisonSamplerId, worldPos, light);

//    // view depth: transform worldPos to view space (RH, forward -Z)
//    float3 viewPos = mul(frame.view, float4(worldPos, 1.0)).xyz;
//    float viewDepth = -viewPos.z; // linear eye depth (positive in front)
//    if (viewDepth < 0)
//        viewDepth = 0;

//    int cascadeIdx = SelectCascadeGpu(viewDepth, dir.cascades, dir.numCascades);
//    GpuCascadeDescription c = dir.cascades[cascadeIdx];

//    uint idx = NonUniformResourceIndex((uint) dir.shadow_map_index);
//    float4 clip = mul(c.lightVP, float4(worldPos, 1.0));
//    float3 ndc = clip.xyz / clip.w;
//    if (ndc.z <= 0.0f || ndc.z >= 1.0f)
//        return 1.0f;
//    float2 uv = ndc.xy * 0.5f + 0.5f;
//    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
//        return 1.0f;

//    // atlas transform
//    float2 uvAtlas = uv * c.uvScale + c.uvBias;
//    // clamp inside tile to avoid bleeding from neighbour cascade
//    uvAtlas = clamp(uvAtlas, c.uvClampMin, c.uvClampMax);

//    float receiverDepth = ndc.z - c.bias; // bias forward (reduce acne)

//    // Use hardware PCF via comparison sampler – single tap with 3x3 via Gather? Keep simple PCFShadow but atlas-aware
//    // PCFShadow samples with texelSize derived from atlas dimensions; for atlas we compute texel from tile
//    //float shadow = PCFShadow(comparisonSamplerId, uvAtlas, receiverDepth, idx);
//    float shadow = PCFShadowAtlas(
//    comparisonSamplerId,
//    uvAtlas,
//    receiverDepth,
//    idx,
//    c.uvClampMin,
//    c.uvClampMax
//);
//    // Alternative hardware:
//    // float shadow = textures[idx].SampleCmpLevelZero(comparisonSamplers[comparisonSamplerId], uvAtlas, receiverDepth);
//    return shadow;
//}

// Legacy wrapper that pulls cascade buffer from push constants – main frag will call this.
// Guarded: deferred-shadow compute passes include this header with their
// own push layout (no scene-wide `pc`; see common.hlsli ENGINE_HAS_PC).
// NOTE: samplerId must be a REGULAR sampler (0): the atlas path does a
// manual SampleLevel(samplers[]) + compare. The comparison sampler (1) is
// only for hardware SampleCmp paths (PCFShadow/GetShadowFactorODB).
#ifdef ENGINE_HAS_PC
float GetShadowFactorCascadedFromPC(
    uint samplerId,
    float3 worldPos,
    float3 faceNormal,
    Light light
)
{
    // BDA discipline (see GetShadowFactorCascaded): pull ONLY the view
    // matrix out of FrameUniforms (one 64-byte physical-storage read vs
    // the old full 1.3 KB materialization) and pass the cascade buffer
    // POINTER + pc scalars — never the 624-byte dir aggregate.
    return GetShadowFactorCascaded(
        samplerId,
        worldPos,
        faceNormal,
        light.direction,
        light.shadow_map_index,
        pc.numCascades,
        pc.frameUniforms.Get().view,
        pc.cascadeBuffer
    );
}
//float GetShadowFactorCascadedFromPC(
//    uint samplerId,
//    float3 worldPos,
//    float3 faceNormal,
//    Light light
//)
//{
//    //return 1.0f;
//    FrameUniforms frame = pc.frameUniforms.Get();
//    GpuDirectionalLightDescription dir = pc.cascadeBuffer.Get().dir;
//    return GetShadowFactorCascaded(samplerId, worldPos, faceNormal, light, frame, dir);
//}
#endif // ENGINE_HAS_PC

// ---- Spot shadows (Forward+ spots, perspective atlas tiles) ----
// tileIndex comes from ForwardPlusLight.shadowTile. Returns 1 (lit) when the
// receiver is outside the spot frustum – so out-of-range pixels are unaffected.
float GetSpotShadowFactorTile(
    uint samplerId,
    float3 worldPos,
    float3 faceNormal,
    float3 spotDirection,
    GpuCascadeDescription t,
    uint atlasIdx
)
{
    // Receiver-side normal bias, same rationale as the cascaded path.
    float3 biasedWorld = worldPos + faceNormal * t.normalBias;
    float4 clip = mul(t.lightVP, float4(biasedWorld, 1.0f));
    // Behind the light (w <= 0) or beyond far plane => lit.
    if (clip.w <= 1e-6f)
        return 1.0f;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z <= 0.0f || ndc.z >= 1.0f)
        return 1.0f;
    float2 uv = ndc.xy * 0.5f + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;

    float2 uvAtlas = uv * t.uvScale + t.uvBias;
    // Sec. 4.2: slope-scaled bias; t.bias is baked per-tile on the CPU.
    float receiverDepth = ndc.z + GetOrientedBias(faceNormal, spotDirection, t.bias, false);
    // parked SSS variant: ndc.z + GetOrientedBias(faceNormal, spotDirection, t.bias, true);
    return PCFShadowAtlas(
        samplerId,
        uvAtlas,
        receiverDepth,
        atlasIdx,
        t.invShadowSize,
        t.uvClampMin,
        t.uvClampMax
    );
}

// PCSS (Fernando 2005) over a perspective spot-shadow atlas tile — the
// deferred-shadow phase 4 visibility technique (paper 2.4; any shadow
// technique substitutes). Steps:
//   1. blocker search: average shadow depths closer than the receiver
//      over a fixed radius (the light's projected size),
//   2. penumbra estimation: width ~ lightSize * (zRecv - zBlocker) /
//      zBlocker, projected into tile UV through the perspective,
//   3. variable-width PCF.
// Eye depth from the RH_ZO NDC: z = far*near / (far - ndc.z*(far-near)).
// All radii are computed in TILE UV and scaled by t.uvScale for the
// atlas; every tap clamps into the tile (same discipline as
// PCFShadowAtlas). outerAngle is the spot's half angle (light struct).
float PcssSpotShadow(
    uint samplerId,
    float3 worldPos,
    float3 faceNormal,
    float3 spotDirection,
    GpuCascadeDescription t,
    uint atlasIdx,
    float lightSize,     // world-space diameter of the emitting sphere
    float nearPlane,     // spot shadow projection near (SPOT_SHADOW_NEAR)
    float outerAngle     // spot half angle (radians)
)
{
    // Same receiver bias + projection as GetSpotShadowFactorTile.
    const float3 biasedWorld = worldPos + faceNormal * t.normalBias;
    const float4 clip = mul(t.lightVP, float4(biasedWorld, 1.0f));
    if (clip.w <= 1e-6f)
        return 1.0f;
    const float3 ndc = clip.xyz / clip.w;
    if (ndc.z <= 0.0f || ndc.z >= 1.0f)
        return 1.0f;
    const float2 uv = ndc.xy * 0.5f + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;
    const float2 uvAtlas = uv * t.uvScale + t.uvBias;
    const float receiverDepth =
        ndc.z + GetOrientedBias(faceNormal, spotDirection, t.bias, false);

    uint aw, ah;
    textures[atlasIdx].GetDimensions(aw, ah);
    const float tileRes = max(float(aw) * t.uvScale.x, 1.0f);
    const float tanOuter = max(tan(min(outerAngle, 1.45f)), 1e-3f);
    const float farP = max(t.splitEnd, nearPlane + 0.01f);
    // Eye distances (RH_ZO perspective inversion; monotone in ndc.z).
    const float zRecv = farP * nearPlane
        / max(farP - ndc.z * (farP - nearPlane), 1e-4f);
    // The frustum face at eye depth z is 2*z*tanOuter wide and the tile
    // spans it: world-to-tile-UV scale = 1 / (2*z*tanOuter).

    // ---- 1) Blocker search (3x3 over the light's projected size) ----
    float rSearchUV = lightSize / max(2.0f * zRecv * tanOuter, 1e-4f);
    rSearchUV = clamp(rSearchUV, 1.5f / tileRes, 12.0f / tileRes);
    float blockerSum = 0.0f;
    uint blockerCount = 0u;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const float2 tapUV = clamp(
                uvAtlas + float2(x, y) * rSearchUV * t.uvScale,
                t.uvClampMin, t.uvClampMax);
            const float d = textures[atlasIdx]
                .SampleLevel(samplers[samplerId], tapUV, 0).r;
            if (d < receiverDepth) { // in front of the receiver: blocker
                blockerSum += d;
                blockerCount += 1u;
            }
        }
    }
    if (blockerCount == 0u)
        return 1.0f; // unoccluded

    // ---- 2) Penumbra estimation (eye-space similar triangles) ----
    const float ndcBlocker = blockerSum / float(blockerCount);
    const float zBlocker = farP * nearPlane
        / max(farP - ndcBlocker * (farP - nearPlane), 1e-4f);
    float rPenUV = lightSize * (zRecv - zBlocker)
        / max(zBlocker * 2.0f * zRecv * tanOuter, 1e-4f);
    rPenUV = clamp(rPenUV, 0.75f / tileRes, 16.0f / tileRes);

    // ---- 3) Variable-width PCF (3x3) ----
    float sum = 0.0f;
    [unroll]
    for (int py = -1; py <= 1; ++py) {
        [unroll]
        for (int px = -1; px <= 1; ++px) {
            const float2 tapUV = clamp(
                uvAtlas + float2(px, py) * rPenUV * t.uvScale,
                t.uvClampMin, t.uvClampMax);
            const float d = textures[atlasIdx]
                .SampleLevel(samplers[samplerId], tapUV, 0).r;
            sum += (receiverDepth <= d) ? 1.0f : 0.0f;
        }
    }
    return sum / 9.0f;
}

// Push-constant wrapper – main frag calls this per shadowed spot.
// Guarded like GetShadowFactorCascadedFromPC above (no `pc` in the
// deferred-shadow compute passes, which call GetSpotShadowFactorTile
// directly through their own push layout).
#ifdef ENGINE_HAS_PC
float GetSpotShadowFactorFromPC(
    uint samplerId,
    float3 worldPos,
    float3 faceNormal,
    float3 spotDirection,
    int tileIndex
)
{
    if (tileIndex < 0)
        return 1.0f;
    if (pc.spotShadowCount == 0 || pc.spotShadowMapIndex == 0xFFFFFFFFu)
        return 1.0f;
    if ((uint) tileIndex >= pc.spotShadowCount)
        return 1.0f;
    if (pc.spotShadowCount > (uint) MAX_SPOT_SHADOWS)
        return 1.0f;
    uint atlasIdx = NonUniformResourceIndex(pc.spotShadowMapIndex);
    GpuCascadeDescription t = pc.spotShadowBuffer.Get().tiles[tileIndex];
    return GetSpotShadowFactorTile(samplerId, worldPos, faceNormal, spotDirection, t, atlasIdx);
}

#endif // ENGINE_HAS_PC

float GetShadowFactorODB(
    uint comparisonSamplerId,
	float3 worldPos,
	Light light,
	float3 normal
)
{
    if (light.shadow_map_index < 0)
        return 1.0f;

    uint idx = NonUniformResourceIndex((uint) light.shadow_map_index);

    float4 clip = mul(light.lightVP, float4(worldPos, 1.0));
    float3 ndc = clip.xyz / clip.w;

	// outside light frustum => lit
    if (ndc.z <= 0.0f || ndc.z >= 1.0f)
        return 1.0f;

    float2 uv = ndc.xy * 0.5f + 0.5f;

    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
		// light on uv outside shadomap
        return 1.0f;

    float receiverDepth = ndc.z;

    // Legacy single-map path has no per-cascade bake; use a tiny fixed
    // NDC base (~5mm at 10m range) with the same slope scaling.
    receiverDepth += GetOrientedBias(
		normal,
		light.direction,
		0.0005f,
		false
	);
    float shadow = textures[idx].SampleCmpLevelZero(
		comparisonSamplers[comparisonSamplerId],
		uv,
		receiverDepth
	);

    return shadow;
}

// ---- EVSM (Exponential Variance Shadow Maps, prefiltered async) ----
// Blurred RG32F moments replace the depth atlas; the tile math (lightVP,
// uvScale/uvBias/uvClamp, oriented bias) is shared with the PCF path.
// evsmAtlasIdx is the bindless index of the BLURRED output (one frame old
// under OneFrameLag pipelining); exponent must match the moment pass
// specialization constant (smvk::kEvsmExponent default 42).
#include "evsm.hlsli"

// BDA discipline (see GetShadowFactorCascaded): same flattened contract —
// scalars the caller already owns (lightDirection/shadowMapIndex from
// Light, numCascades from pc), the one view matrix used, and the cascade
// buffer POINTER instead of FrameUniforms/GpuDirectionalLightDescription
// by value. SampleEvsmTileSoft likewise takes (cb, cascadeIndex).
float GetShadowFactorEvsmCascaded(
    uint samplerId,
    float3 worldPos,
    float3 faceNormal,
    float3 lightDirection,
    int shadowMapIndex,
    uint numCascades,
    float4x4 view,
    vk::BufferPointer<DirectionalCascadeBuffer> cb,
    uint evsmAtlasIdx,
    float exponent,
    float2 pix)
{
    if (shadowMapIndex < 0 || numCascades == 0u)
        return 1.0f;

    float3 viewPos = mul(view, float4(worldPos, 1.0f)).xyz;
    float viewDepth = max(-viewPos.z, 0.0f);
    int firstCascade = SelectCascadeGpu(viewDepth, cb, numCascades);

    // First valid cascade at/after the view-depth selection (same fallthrough
    // as the PCF path: avoids a hard pop to unshadowed when the selected
    // tile misses the receiver).
    int usedK = -1;
    float shadow = 1.0f;
    for (int k = firstCascade; k < int(numCascades); ++k)
    {
        bool valid = false;
        float s = SampleEvsmTileSoft(
            cb, (uint) k, worldPos, faceNormal, lightDirection,
            samplerId, evsmAtlasIdx, exponent, pix, valid);
        if (valid)
        {
            usedK = k;
            shadow = s;
            break;
        }
    }
    if (usedK < 0)
        return 1.0f;

    // Border blend into the next cascade over the last 10% of this cascade's
    // view-depth band (mirrors GetShadowFactorCascaded): hides the
    // shadow-density step where texel size and baked bias change. splitEnd
    // reads stay scalar (BDA discipline: no cascade aggregate copies).
    if (usedK + 1 < int(numCascades))
    {
        float splitEndK = cb.Get().dir.cascades[usedK].splitEnd;
        float splitStart =
            (usedK == 0)
                ? 0.0f
                : cb.Get().dir.cascades[usedK - 1].splitEnd;
        float bandW = (splitEndK - splitStart) * 0.1f;
        if (bandW > 1e-4f && viewDepth > splitEndK - bandW)
        {
            float t = smoothstep(splitEndK - bandW, splitEndK, viewDepth);
            bool nextValid = false;
            float nextShadow = SampleEvsmTileSoft(
                cb, (uint) (usedK + 1), worldPos, faceNormal, lightDirection,
                samplerId, evsmAtlasIdx, exponent, pix, nextValid);
            if (nextValid)
                shadow = lerp(shadow, nextShadow, t);
        }
    }
    return shadow;
}

float GetSpotShadowFactorEvsmTile(
    uint samplerId,
    float3 worldPos,
    float3 faceNormal,
    float3 spotDirection,
    float outerAngle,
    GpuCascadeDescription t,
    uint evsmAtlasIdx,
    float exponent,
    float2 pix)
{
    if (evsmAtlasIdx == 0xFFFFFFFFu)
        return 1.0f;
    bool valid = false;
    // Soft spot shadows (PCSS over prefiltered moments): outer angle comes
    // from the light; near/lightSize/softness ride the tile pads.
    float s = SampleEvsmSpotTileSoft(
        t, worldPos, faceNormal, spotDirection, outerAngle,
        samplerId, evsmAtlasIdx, exponent, pix, valid);
    return valid ? s : 1.0f;
}

#ifdef ENGINE_HAS_PC
float GetShadowFactorEvsmCascadedFromPC(
    uint samplerId,
    float3 worldPos,
    float3 faceNormal,
    Light light,
    uint evsmAtlasIdx,
    float exponent,
    float2 pix)
{
    // BDA discipline (see GetShadowFactorCascadedFromPC): only the view
    // matrix out of FrameUniforms + the cascade buffer POINTER + pc
    // scalars — no 1.3 KB/624 B aggregate materializations.
    return GetShadowFactorEvsmCascaded(
        samplerId, worldPos, faceNormal,
        light.direction, light.shadow_map_index,
        pc.numCascades, pc.frameUniforms.Get().view, pc.cascadeBuffer,
        evsmAtlasIdx, exponent, pix);
}

float GetSpotShadowFactorEvsmFromPC(
    uint samplerId,
    float3 worldPos,
    float3 faceNormal,
    float3 spotDirection,
    float outerAngle,
    int tileIndex,
    uint evsmAtlasIdx,
    float exponent,
    float2 pix)
{
    if (tileIndex < 0 || evsmAtlasIdx == 0xFFFFFFFFu)
        return 1.0f;
    if (pc.spotShadowCount == 0u)
        return 1.0f;
    if ((uint)tileIndex >= pc.spotShadowCount)
        return 1.0f;
    GpuCascadeDescription t = pc.spotShadowBuffer.Get().tiles[tileIndex];
    return GetSpotShadowFactorEvsmTile(
        samplerId, worldPos, faceNormal, spotDirection, outerAngle, t,
        evsmAtlasIdx, exponent, pix);
}
#endif // ENGINE_HAS_PC
