#pragma once


#ifndef NUM_CASCADES
#define NUM_CASCADES 3
#endif

struct CascadeData
{
	float splitEnd;
	float3 _pad;
};

int SelectCascade(float viewDepth, CascadeData cascades[NUM_CASCADES])
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

