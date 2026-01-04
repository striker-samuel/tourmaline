#pragma once


#ifndef NUM_CASCADES
#define NUM_CASCADES 3
#endif

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

float PCFShadow(
	Texture2D shadowMaps[],      // descriptor array
	SamplerState shadowSamplers[],// descriptor array
	float2 uv,
	float currentDepth,
	uint shadowIdx
)
{


//	float2 invShadowSize; // Texel Size Optimization
	uint w, h;
	shadowMaps[shadowIdx].GetDimensions(w, h);
	float2 texelSize = 1.0 / float2(w, h);

	float sum = 0.0;
	int samples = 0;

	[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			[unroll]
			for (int y = -1; y <= 1; ++y)
			{
				float2 offset = float2(x, y) * texelSize;

				float d = shadowMaps[shadowIdx]
					.Sample(shadowSamplers[shadowIdx], uv + offset)
					.r;

				const float bias = 0.001f;
				sum += (currentDepth - bias <= d) ? 1.0f : 0.0f;
				samples++;
			}
		}

	return sum / samples;
}

float GetShadowFactor(
	Texture2D shadowMaps[],
	SamplerState shadowSamplers[],
	float3 worldPos,
	Light light
)
{
	if (light.shadow_map_index < 0)
		return 1.0f;

	uint idx = NonUniformResourceIndex((uint)light.shadow_map_index);

	float4 clip = mul(light.lightVP, float4(worldPos, 1.0));
	float3 ndc = clip.xyz / clip.w;

	if (ndc.z <= 0.0f || ndc.z >= 1.0f)
		return 1.0f;

	float2 uv = ndc.xy * 0.5f + 0.5f;

	if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
		return 1.0f;

	return PCFShadow(shadowMaps, shadowSamplers, uv, ndc.z, idx);
}
//
//// Directional lights need cascaeds for better results.
//float GetCascadeShadowFactor(
//	Texture2D shadowMaps[],
//	SamplerState shadowSamplers[],
//	CascadeData cascades[],
//	float viewDepth,
//	float3 worldPos
//)
//{
//	int c = SelectCascade(viewDepth, cascades);
//
//	// If descriptor indexing is used, this is good practice:
//	uint idx = NonUniformResourceIndex((uint)c);
//
//	float4 clip = mul(cascades[c].lightVP, float4(worldPos, 1.0));
//	float3 ndc = clip.xyz / clip.w;
//
//	if (ndc.z <= 0.0f || ndc.z >= 1.0f) return 1.0f;
//
//	float2 uv = ndc.xy * 0.5f + 0.5f;
//	if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) return 1.0f;
//
//	return PCFShadow(shadowMaps, shadowSamplers, uv, ndc.z, idx);
//}


//float SampleDirectionalCSM(float3 worldPos, float viewDepth)
//{
//	int c = SelectCascade(viewDepth);
//	CascadeMeta m = gCascades[c];
//
//	float4 clip = mul(m.lightVP, float4(worldPos, 1));
//	float3 ndc = clip.xyz / clip.w;
//
//	// directional is usually orthographic, but this still works
//	if (ndc.z <= 0 || ndc.z >= 1) return 1;
//
//	float2 uv = ndc.xy * 0.5f + 0.5f;
//	if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return 1;
//
//	// remap to atlas tile
//	float2 uvAtlas = uv * m.uvScale + m.uvBias;
//	uvAtlas = clamp(uvAtlas, m.uvClampMin, m.uvClampMax);
//
//	// PCF with invAtlasSize (or invShadowSize)
//	return PCFAtlas(gDirShadowAtlas, gDirShadowSampler, uvAtlas, ndc.z, m);
//}
