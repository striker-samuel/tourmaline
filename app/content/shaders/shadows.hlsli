#pragma once


#ifndef NUM_CASCADES
#define NUM_CASCADES 3
#endif

#define Epsilon 1
#include "light.hlsli"

struct CascadeMeta
{
	float4x4 lightVP;
	float splitEnd;        // view-space depth end for this cascade
	float bias;
	float normalBias;

	float2 invShadowSize;  // 1/(atlasWidth, atlasHeight) OR 1/(tileWidth,tileHeight) depending how you do PCF
	float2 uvScale;        // tile scale in atlas UV
	float2 uvBias;         // tile offset in atlas UV
	float2 uvClampMin;     // clamp inside tile (avoid bleeding)
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

//float PCFShadow(
//	Texture2D shadowMaps[],
//	SamplerComparisonState  shadowSamplers[],
//	float2 uv,
//	float currentDepth,   // receiver depth in shadow map depth space (0..1)
//	uint shadowIdx
//)
//{
//	uint w, h;
//	shadowMaps[shadowIdx].GetDimensions(w, h);
//	float2 texelSize = 1.0f / float2(w, h);
//
//	// Kernel radius in texels:
//	// 0 => 1x1
//	// 1 => 3x3
//	// 2 => 5x5
//	// 3 => 7x7
//	const int radius = 1; // <-- set what you actually want
//
//	float sum = 0.0f;
//	int samples = 0;
//
//	[unroll]
//		for (int y = -radius; y <= radius; ++y)
//		{
//			[unroll]
//			for (int x = -radius; x <= radius; ++x)
//			{
//				float2 offset = float2(x, y) * texelSize;
//
//				float d = shadowMaps[shadowIdx]
//					.Sample(shadowSamplers[shadowIdx], uv + offset)
//					.r;
//
//				sum += (currentDepth <= d) ? 1.0f : 0.0f;
//				samples++;
//			}
//		}
//
//	return sum / samples;
//}

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


//11 float3 GetLightDirection(in LightInfo lightInfo,
//    12 in float3 worldPos)
//    13 {
//    14 return lightInfo.IsDirectional() ? lightInfo.LightDirection
//        15 : worldPos - lightInfo.WorldPos;
//    16 }


float GetShadowFactor(
	float3 worldPos,
	Light light
)
{
	if (light.shadow_map_index < 0)
		return 1.0f;
	
	
	uint idx = NonUniformResourceIndex((uint)light.shadow_map_index);
	
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

	float shadow = textures[idx].SampleCmpLevelZero(
		comparisonSamplers[comparisonSamplerId],
		uv,
		receiverDepth
	);

	return shadow;
}

float GetShadowFactorODB(
	Texture2D shadowMaps[],
	SamplerComparisonState shadowSamplers[],
	float3 worldPos,
	Light light,
	float3 normal
)
{
	if (light.shadow_map_index < 0)
		return 1.0f;

	uint idx = NonUniformResourceIndex((uint)light.shadow_map_index);

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

	float shadow = shadowMaps[idx].SampleCmpLevelZero(
		shadowSamplers[idx],
		uv,
		receiverDepth
	);

	return shadow;
}
