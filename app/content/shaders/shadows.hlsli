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

float GetOrientedBias(float3 faceNormal,
	float3 lightDirection,
	bool isSSS
)
{
    static float kOrientedBias = 0.00002f;
    float isFacingLight = dot(faceNormal, lightDirection) < 0;
    bool moveTowardLight = isSSS || isFacingLight;
    return moveTowardLight ? -kOrientedBias : kOrientedBias;
}


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


float GetShadowFactorCascaded(
    uint samplerId,
    float3 worldPos,
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
        return GetShadowFactor(
            samplerId,
            worldPos,
            light
        );
    }

    float3 viewPos =
        mul(
            frame.view,
            float4(worldPos, 1.0f)
        ).xyz;

    float viewDepth =
        max(-viewPos.z, 0.0f);

    int cascadeIdx =
        SelectCascadeGpu(
            viewDepth,
            dir.cascades,
            dir.numCascades
        );

    GpuCascadeDescription c =
        dir.cascades[cascadeIdx];

    uint idx =
        NonUniformResourceIndex(
            (uint) dir.shadow_map_index
        );

    float4 clip =
        mul(
            c.lightVP,
            float4(worldPos, 1.0f)
        );

    float3 ndc =
        clip.xyz / clip.w;

    float2 uv =
        ndc.xy * 0.5f + 0.5f;

    // Invalid selected cascade.
    //
    // If this happens often, fix the CPU-side
    // cascade bounds/padding.
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

    float receiverDepth =
        ndc.z - c.bias;

    return PCFShadowAtlas(
        samplerId,
        uvAtlas,
        receiverDepth,
        idx,
        c.uvClampMin,
        c.uvClampMax
    );
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
    Light light
)
{
    FrameUniforms frame = pc.frameUniforms.Get();
    GpuDirectionalLightDescription dir = pc.cascadeBuffer.Get().dir;
    return GetShadowFactorCascaded(comparisonSamplerId, worldPos, light, frame, dir);
}

// ---- Spot shadows (Forward+ spots, perspective atlas tiles) ----
// tileIndex comes from ForwardPlusLight.shadowTile. Returns 1 (lit) when the
// receiver is outside the spot frustum – so out-of-range pixels are unaffected.
float GetSpotShadowFactorTile(
    uint samplerId,
    float3 worldPos,
    GpuCascadeDescription t,
    uint atlasIdx
)
{
    float4 clip = mul(t.lightVP, float4(worldPos, 1.0f));
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
    float receiverDepth = ndc.z - t.bias;
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
    return GetSpotShadowFactorTile(samplerId, worldPos, t, atlasIdx);
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
	
    receiverDepth += GetOrientedBias(
		normal,
		light.direction,
		false
	);
    float shadow = textures[idx].SampleCmpLevelZero(
		comparisonSamplers[comparisonSamplerId],
		uv,
		receiverDepth
	);

    return shadow;
}
