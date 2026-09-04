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
		int shadowMapRes
	);
	glm::mat4 GetPerspectiveProjectionForLight(
		const SpotLight& l,
		float aspect = 1.0f,
		float nearPlane = 0.1f,
		float farPlane = 100.0f
	);

};
