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
int SelectCascadeGpu(
    float viewDepth,
    GpuCascadeDescription cascades[4],
    int numCascades)
{
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        if (i >= numCascades)
            break;

        if (viewDepth <= cascades[i].splitEnd)
            return i;
    }

    return numCascades - 1;
}

//// GpuCascade overload – selects on GpuDirectionalLightDescription
//int SelectCascadeGpu(float viewDepth, GpuCascadeDescription cascades[4], int numCascades)
//{
//    int firstCascade =
//    SelectCascadeGpu(
//        viewDepth,
//        dir.cascades,
//        dir.numCascades
//    );

//    uint idx =
//    NonUniformResourceIndex(
//        (uint) dir.shadow_map_index
//    );

//[loop]
//    for (
//    int cascadeIdx = firstCascade;
//    cascadeIdx < dir.numCascades;
//    ++cascadeIdx
//)
//    {
//        GpuCascadeDescription c =
//        dir.cascades[cascadeIdx];

//        float4 clip =
//        mul(
//            c.lightVP,
//            float4(worldPos, 1.0f)
//        );

//        float3 ndc = clip.xyz / clip.w;

//        float2 uv =
//        ndc.xy * 0.5f + 0.5f;

//        bool inside =
//        ndc.z > 0.0f &&
//        ndc.z < 1.0f &&
//        uv.x >= 0.0f &&
//        uv.x <= 1.0f &&
//        uv.y >= 0.0f &&
//        uv.y <= 1.0f;

//        if (!inside)
//            continue;

//        float2 uvAtlas =
//        uv * c.uvScale + c.uvBias;

        
        
//        float receiverDepth = ndc.z; // bias from sampler
//        //float receiverDepth =
//        //ndc.z - c.bias;

//        return PCFShadowAtlas(
//        comparisonSamplerId,
//        uvAtlas,
//        receiverDepth,
//        idx,
//        c.uvClampMin,
//        c.uvClampMax
//    );
//    }

//    //return 1.0f;
//    //[unroll]
//    //for (int i = 0; i < 4; ++i)
//    //{
//    //    if (i >= numCascades)
//    //        break;
//    //    if (viewDepth < cascades[i].splitEnd)
//    //        return i;
//    //}
//    //return numCascades - 1;
//}

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
    float2 uvClampMin,
    float2 uvClampMax
)
{
    uint w, h;
    textures[shadowIdx].GetDimensions(w, h);

    float2 texelSize = 1.0f / float2(w, h);

    const int radius = 1;

    float sum = 0.0f;
    int samples = 0;

    [unroll]
    for (int y = -radius; y <= radius; ++y)
    {
        [unroll]
        for (int x = -radius; x <= radius; ++x)
        {
            float2 offset = float2(x, y) * texelSize;

            // IMPORTANT: clamp every PCF tap,
            // not just the center coordinate.
            float2 sampleUV = clamp(
                uv + offset,
                uvClampMin,
                uvClampMax
            );

            float d = textures[shadowIdx]
                .Sample(
                    samplers[samplerId],
                    sampleUV
                )
                .r;

            sum += currentDepth <= d ? 1.0f : 0.0f;
            samples++;
        }
    }

    return sum / samples;
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


// PARKED (legacy single-map path, kept in code): single lightVP PCF without
// oriented bias. Not called while cascades are active.
/*
float GetShadowFactor(
    uint comparisonSamplerId,
	float3 worldPos,
	Light light
)
{
    if (light.shadow_map_index < 0)
        return 1.0f;
    uint idx = NonUniformResourceIndex((uint) light.shadow_map_index);
    float4 clip = mul(light.lightVP, float4(worldPos, 1.0));
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z <= 0.0f || ndc.z >= 1.0f)
        return 1.0f;
    float2 uv = ndc.xy * 0.5f + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;
    return PCFShadow(1, uv, ndc.z, idx);
}
*/


// Samples one cascade tile; valid=false when the (normal-biased) receiver
// falls outside the cascade light frustum so the caller can fall through
// to the next cascade instead of popping to unshadowed.
float SampleCascadeTile(
    GpuCascadeDescription c,
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
        worldPos + faceNormal * c.normalBias;

    float4 clip =
        mul(
            c.lightVP,
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
        uv * c.uvScale +
        c.uvBias;

    // Sec. 4.2: slope-scaled NDC bias; c.bias is the per-cascade
    // worldBias/depthRange baked on the CPU so far cascades don't
    // over-bias (peter-panning) while near ones under-bias (acne).
    float receiverDepth =
        ndc.z + GetOrientedBias(faceNormal, lightDirection, c.bias, false);
        // parked SSS variant: ndc.z + GetOrientedBias(faceNormal, lightDirection, c.bias, true);

    valid = true;
    return PCFShadowAtlas(
        samplerId,
        uvAtlas,
        receiverDepth,
        idx,
        c.uvClampMin,
        c.uvClampMax
    );
}

float GetShadowFactorCascaded(
    uint samplerId,
    float3 worldPos,
    float3 faceNormal,
    Light light,
    FrameUniforms frame,
    GpuDirectionalLightDescription dir
)
{
    if (
        light.shadow_map_index < 0 ||
        dir.numCascades <= 0
    )
    {
        // Legacy single-map fallback is parked (see GetShadowFactor above).
        return 1.0f;
    }

    float3 viewPos =
        mul(
            frame.view,
            float4(worldPos, 1.0f)
        ).xyz;

    float viewDepth =
        max(-viewPos.z, 0.0f);

    int firstCascade =
        SelectCascadeGpu(
            viewDepth,
            dir.cascades,
            dir.numCascades
        );

    uint idx =
        NonUniformResourceIndex(
            (uint) dir.shadow_map_index
        );

    // First valid cascade at/after the view-depth selection. Falling
    // through (instead of returning lit when the selected tile misses)
    // removes the hard pop from "fully shadowed" to "barely shadowed"
    // when a receiver sits outside the selected cascade's light bounds.
    int usedK = -1;
    float shadow = 1.0f;
    for (int k = firstCascade; k < dir.numCascades; ++k)
    {
        bool valid = false;
        float s = SampleCascadeTile(
            dir.cascades[k],
            worldPos,
            faceNormal,
            light.direction,
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
    // reads as a shadow-density step; lerping hides the seam.
    if (usedK + 1 < dir.numCascades)
    {
        GpuCascadeDescription c = dir.cascades[usedK];
        float splitStart = (usedK == 0) ? 0.0f : dir.cascades[usedK - 1].splitEnd;
        float bandW = (c.splitEnd - splitStart) * 0.1f;
        if (bandW > 1e-4f && viewDepth > c.splitEnd - bandW)
        {
            float t = smoothstep(c.splitEnd - bandW, c.splitEnd, viewDepth);
            bool nextValid = false;
            float nextShadow = SampleCascadeTile(
                dir.cascades[usedK + 1],
                worldPos,
                faceNormal,
                light.direction,
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

// Legacy wrapper that pulls cascade buffer from push constants – main frag will call this
float GetShadowFactorCascadedFromPC(
    uint comparisonSamplerId,
    float3 worldPos,
    float3 faceNormal,
    Light light
)
{
    FrameUniforms frame = pc.frameUniforms.Get();
    GpuDirectionalLightDescription dir = pc.cascadeBuffer.Get().dir;
    return GetShadowFactorCascaded(comparisonSamplerId, worldPos, faceNormal, light, frame, dir);
}

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
        t.uvClampMin,
        t.uvClampMax
    );
}

// Push-constant wrapper – main frag calls this per shadowed spot.
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
