// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <glm/glm.hpp>

namespace smvk {

	// ──────────────────────────────────────────────────────────────────────────
	// Light intensity – units & photometric interpretation
	// ──────────────────────────────────────────────────────────────────────────
	// All lights store `color` (linear RGB, not sRGB) and `intensity` (scalar
	// multiplier). The product `color.rgb * intensity` is the emitted radiance
	// / irradiance used in the shader. The *units* depend on light type:
	//
	//  • DirectionalLight (sun / moon):
	//      intensity = illuminance in **lux** (lumens / m²) incident on a
	//      surface with N·L = 1. Shader computes `N·L * color * intensity`
	//      without distance falloff. Physically, midday sun ≈ 100 000 lux,
	//      overcast ≈ 10 000 lux, indoor ≈ 200–800 lux. For an LDR swapchain
	//      without tonemapping 1.0 ≈ 1 lux looks "unit" bright; after the
	//      HDR + ACES tonemapper you can use physical values and control
	//      overall brightness via FrameUniforms.exposure (EV). Current demo
	//      uses intensity = 10 for a low-dynamic-range look; raise to
	//      5000–10000 when tonemapper + exposure = 1 is enabled.
	//
	//  • PointLight:
	//      intensity = luminous intensity in **candela** (lumens / steradian).
	//      Shader should apply inverse-square attenuation
	//      `atten = intensity / (4π·r²)` (or `1/r²` simplified) so that
	//      `radiance = color * atten`. A 60 W bulb ≈ 800 lm ≈ 64 cd
	//      isotropic. Range is encoded in attenuation; no explicit radius –
	//      shader clamps at `r² = max(r², 0.01)`.
	//
	//  • SpotLight:
	//      intensity = same candela as PointLight, multiplied by a cone
	//      falloff `smoothstep(outerAngle, innerAngle, spotCos)`. innerAngle
	//      = fully lit cone, outerAngle = falloff edge (radians). Both
	//      measured from the spot axis (`-direction`).
	//
	//  • General:
	//      color is typically warm/cool white (e.g. 6500 K ≈ (1.0, 0.95, 0.92)).
	//      Keep color * intensity in linear space; the final tonemap pass
	//      (ACES / Reinhard) + gamma converts HDR linear → LDR sRGB. If you
	//      bypass tonemapping, treat intensity as an LDR “brightness” knob
	//      (0…10) rather than physical lux/cd.
	// ──────────────────────────────────────────────────────────────────────────

	struct PointLight {
		glm::vec4 color = glm::vec4(1.0f);
		glm::vec3 position = { 0.0f, 0.0f, 0.0f };
		int32_t flags = 0;
		// candela (lm/sr); see header doc. Typical  10–500 for indoor, 800 for bulb
		float intensity = .0f;
		int shadowSlot = -1;
	};

	struct SpotLight {
		glm::vec4 color = glm::vec4(1.0f);
		glm::vec3 position = { 0.0f, 0.0f, 0.0f };
		glm::vec3 direction = { 0.0f,0.0f,0.0f };
		int32_t flags = 0;
		// candela; angular falloff between innerAngle/outerAngle (radians)
		float intensity = .0f;
		float innerAngle = glm::radians(10.0f);
		float outerAngle = glm::radians(20.0f);
		int shadowSlot = -1;
	};

	struct DirectionalLight {
		glm::vec4 color = glm::vec4(1.0f);
		// No use now but maybe used for masks 
		int32_t flags = 0;
		float direction[3] = { 0.0f,0.0f,0.0f };
		// lux (lm/m²) for N·L=1; see header doc. Sun ≈ 50k–100k with tonemapper
		float intensity = .0f;
		int shadowSlot = -1;
	};

	struct alignas(16) Light {
		glm::vec4 color = glm::vec4(1.0f);
		float position[3] = { 0.0f,0.0f,0.0f };
		int32_t flags = 0;
		float direction[3] = { 0.0f,0.0f,0.0f };
		// Unified field – units depend on Light.type / shader path:
		//  Directional → lux, Point/Spot → candela; see top-of-file doc.
		//  Shader multiplies color.rgb * intensity before tonemap.
		float intensity = .0f;
		int32_t pad_;
		float innerAngle = glm::radians(10.0f);
		float outerAngle = glm::radians(20.0f);
		int shadow_map_index = -1;
		float lightVP[16] = {};
	};

	static_assert(sizeof(Light) % 16 == 0);

	// -------------- GPU descriptions ---------------

	struct GpuCascadeDescription
	{
		float lightVP[16];
		float splitEnd;
		float bias;
		float normalBias;
		float pad_;
		glm::vec2 invShadowSize;
		glm::vec2 uvScale;
		glm::vec2 uvBias;
		glm::vec2 uvClampMin;
		glm::vec2 uvClampMax;
		float pad2_[2];
	};
	static_assert(sizeof(GpuCascadeDescription) % 16 == 0);

	// Lets just hardcode the different types of lights. 
	// they do need different data and are implemented quiter differently. Data is king, so from now on they are separated. 
	struct alignas(16) GpuDirectionalLightDescription
	{
		glm::vec4 color = glm::vec4(1.0f);
		float direction[3] = { 0.0f,0.0f,0.0f };
		int32_t flags = 0;
		float intensity = .0f;
		int shadow_map_index = -1;
		int numCascades = 1;
		float pad_ = 0.0f; // scalar pad: keeps lightVP 16-byte aligned (no straddling vec3)
		float lightVP[16] = {};
		GpuCascadeDescription cascades[4];
	};
	static_assert(sizeof(GpuDirectionalLightDescription) == 624);


	struct alignas(16) GpuSpotLightDescription {
		glm::vec4 color = glm::vec4(1.0f);
		float position[3] = { 0.0f,0.0f,0.0f };
		int32_t flags = 0;
		float direction[3] = { 0.0f,0.0f,0.0f };
		float intensity = .0f;
		float _pad1;
		float innerAngle = glm::radians(10.0f);
		float outerAngle = glm::radians(20.0f);
		int shadow_map_index = -1;
		float lightVP[16] = {};
	};
	static_assert(sizeof(GpuSpotLightDescription) % 16 == 0);

	glm::mat4 GetSpotLightView(
		const SpotLight& l
	);
	// Perspective view-projection for a spot shadow map (Vulkan RH ZO,
	// depth [0,1]). Shares the no-Y-flip convention with the directional
	// path: the same matrix is used for rendering and sampling, so the
	// NDC->UV convention cancels out. fov = outerAngle*2, far = range.
	glm::mat4 GetSpotLightVP(
		const glm::vec3& position,
		const glm::vec3& direction,
		float outerAngle,
		float range,
		float nearPlane = 0.5f,
		float aspect = 1.0f
	);
	// Point-light shadow face VP: one of the six 90-degree perspective
	// frustums that tile the sphere around a point light (the "cube" of a
	// cube-map shadow, rasterized as six atlas TILES - see spotshadow.h
	// BuildPointFaceTile). `face` is 0..5 in CubemapFace order
	// (+X, -X, +Y, -Y, +Z, -Z) and the per-face basis (forward/up) is the
	// engine cubemap convention (cubemap.h FaceUvToDirection), so a
	// direction whose dominant axis is the face axis projects to NDC
	// |xy| <= 1 EXACTLY (each NDC axis is one non-dominant component over
	// the dominant one - never a mix) and the NDC square matches the
	// cubemap face's (su, sv). The sampling side (shadows.hlsli
	// GetPointShadowFactorTile) picks the face by dominant axis the same
	// way cubemap_prefilter DirectionToFaceUv does, then projects through
	// the SAME matrix from the tile descriptor - raster and sample share
	// one VP per face, the same contract as the spot/cascade paths.
	glm::mat4 GetPointShadowFaceVP(
		const glm::vec3& position,
		uint32_t face,
		float range,
		float nearPlane = 0.5f
	);
	// LiSPSM (Light Space Perspective Shadow Maps) for spotlights.
	// Warps the spot projection to match the camera's perspective frustum,
	// providing better texel density in camera/view space.
	// Based on LiSPSM (Light Space Perspective Shadow Maps) by Wimmer et al.
	// Returns the LiSPSM view-projection matrix for the spot light.
	glm::mat4 GetSpotLightVPLiSPSM(
		const glm::vec3& position,
		const glm::vec3& direction,
		float outerAngle,
		float range,
		float nearPlane,
		float aspect,
		const glm::mat4& cameraView,
		const glm::mat4& cameraProj,
		float cameraNear,
		float cameraFar
	);
	// Optional caster-aware cascade fit input (GetDirectionalLightOrthoVP):
	// the merged world-space AABB of the shadow-casting CANDIDATE geometry —
	// the same flag-qualified set the shadow draws iterate (everything the
	// contribution cull may retain). invalid/absent = the slice-only fit.
	struct ShadowCasterFitBounds {
		glm::vec3 min{ 0.0f };
		glm::vec3 max{ 0.0f };
		bool valid = false;
	};
	// The explicit cap on how far BEYOND the receiver slice's own extent
	// the directional shadow map's depth range reaches to cover casters
	// (the documented pop bound: a caster farther sunward than this past
	// the slice clips out of the map, by decision rather than by luck).
	// D32 + the per-cascade bias dividing by the ACTUAL depthRange keep a
	// wider range bias-corrected, so the cost of the coverage is size
	// alone.
	inline constexpr float kDirectionalShadowMaxDistance = 500.0f;

	void GetDirectionalLightOrthoVP(
		const glm::vec3 lightDir,
		const glm::mat4& invCameraView,
		const glm::mat4& invCameraProj,
		float shadowNear,
		float shadowFar,
		glm::mat4& outView,
		glm::mat4& outProj,
		float zPadding,
		bool enableTexelSnap,
		int shadowMapRes,
		// Optional: receives the ortho depth range (zFar - zNear, world units)
		// so callers can scale NDC-space biases (e.g. oriented depth bias
		// magnitude = worldBias / range). May be nullptr.
		float* outDepthRange = nullptr,
		// Optional caster-aware DEPTH coverage (the P2 coverage gap): the
		// contribution view retains casters arbitrarily far sunward, but
		// the slice-only fit clipped them at the light's near plane — a
		// tall building up-sun passed the cull and its shadow still
		// popped. When provided (valid), the light backs off and the
		// depth range extends to cover the casters' extent along the
		// light axis, clamped by maxShadowDistance. The XY window stays
		// slice-fit: light-space xy of a caster whose shadow reaches the
		// slice equals its receiver's (translation along the light moves
		// depth only), so the slice window already contains every
		// relevant caster's xy — depth was the only hole.
		const ShadowCasterFitBounds* casterBounds = nullptr,
		float maxShadowDistance = kDirectionalShadowMaxDistance
	);
	glm::mat4 GetPerspectiveProjectionForLight(
		const SpotLight& l,
		float aspect = 1.0f,
		float nearPlane = 0.1f,
		float farPlane = 100.0f
	);

};
