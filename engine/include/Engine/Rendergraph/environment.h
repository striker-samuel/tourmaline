// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Configurable skydome (visible sky) + analytic IBL + distance/height fog.
//
// Design:
//  - All environment state rides in FrameUniforms.env (see EnvironmentFrameData)
//    so no extra buffers or push-constant growth is needed. Colors are stored
//    as linear 0..1 RGB plus a dedicated HDR intensity; the product is the
//    pre-exposure HDR radiance sampled by the sky pass and the PBR IBL.
//  - The sun direction in env is the single source of truth: the app drives
//    both the directional light and the sky sun disc from it.
//  - Skydome is drawn as the first fullscreen-triangle draw inside the Main
//    pass (depth test Always, no writes) so it works for both the 1x HDR and
//    the 2x MSAA-resolve paths without extra passes or depth tricks.
//
// Frame packing (8 x float4 = 128 bytes, 16-byte aligned):
//   skyZenith      rgb = zenith color (linear 0..1)      w = skyIntensity (HDR multiplier)
//   skyHorizon     rgb = horizon color (linear 0..1)     w = gradient exponent
//   ground         rgb = ground color (linear 0..1)      w = iblIntensity (diffuse/spec multiplier)
//   sunDir         xyz = direction TO sun (normalized)   w = sun disc cos(angularRadius)
//   sunColor       rgb = sun color (linear 0..1)         w = sunIntensity (HDR, lux-like)
//   fogColor       rgb = fog color (linear 0..1)         w = fogIntensity (HDR multiplier)
//   fogDensity     x = exp2 density  y = fog height  z = height falloff  w = linear start
//   fogEndAndFlags x = linear end  y = skyMode  z = fogMode  w = iblEnabled (0/1)
//
// Modes:
//   skyMode 0 = off (clear color shows), 1 = gradient + sun disc
//   fogMode 0 = off, 1 = exp2 distance, 2 = linear distance
//   Height fog multiplies the distance factor by exp(-(y - h) * falloff) for
//   pixels above h (and 1.0 below h when falloff > 0, disabled when falloff == 0).
//
// Usage (app/src/main.cpp):
//   smvk::EnvironmentSettings env;            // CPU-editable, ImGui-driven
//   FrameUniforms::env = env.ToFrameData();   // per frame
//   smvk::Skydome sky;                        // program owner
//   sky.Ensure(ctx);                          // compile sky.vert/sky.frag
//   // inside Main recording func, first draw:
//   cmd.BindRenderPipeline(sky.program, Empty, Disabled(), ...);
//   cmd.PushConstants(... pc ...);
//   cmd.Draw(3, 1, 0, 0);

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstdint>

namespace smvk {

// Must match HLSL EnvironmentFrameData in common.hlsli (field order + size).
struct alignas(16) EnvironmentFrameData {
	glm::vec4 skyZenith{ 0.25f, 0.45f, 0.90f, 600.0f };
	glm::vec4 skyHorizon{ 0.65f, 0.75f, 0.90f, 2.0f };
	glm::vec4 ground{ 0.20f, 0.18f, 0.15f, 20.0f };
	glm::vec4 sunDirection{ 0.0f, 1.0f, 0.0f, 0.9998f };
	glm::vec4 sunColor{ 1.0f, 0.95f, 0.90f, 10000.0f };
	glm::vec4 fogColor{ 0.65f, 0.75f, 0.90f, 600.0f };
	glm::vec4 fogDensityHeight{ 0.01f, 0.0f, 0.0f, 10.0f };
	glm::vec4 fogEndAndFlags{ 80.0f, 1.0f, 0.0f, 1.0f };
};
static_assert(sizeof(EnvironmentFrameData) == 128);
static_assert(sizeof(EnvironmentFrameData) % 16 == 0);

inline constexpr float kSkyOff = 0.0f;
inline constexpr float kSkyGradient = 1.0f;

inline constexpr float kFogOff = 0.0f;
inline constexpr float kFogExp2 = 1.0f;
inline constexpr float kFogLinear = 2.0f;

// CPU-editable settings. ImGui writes these; ToFrameData() packs the GPU form.
struct EnvironmentSettings {
	bool skyEnabled = true;
	bool iblEnabled = true;

	glm::vec3 skyZenithColor{ 0.25f, 0.45f, 0.90f };
	float skyIntensity = 600.0f; // HDR multiplier for the visible dome
	float skyExponent = 2.0f;    // horizon->zenith gradient pow

	glm::vec3 skyHorizonColor{ 0.65f, 0.75f, 0.90f };
	glm::vec3 groundColor{ 0.20f, 0.18f, 0.15f };
	float iblIntensity = 20.0f; // HDR multiplier for diffuse/spec IBL

	// Sun: also drives the directional light when the app links them.
	float sunAzimuthDeg = 30.0f;   // compass around Y, 0 = +Z
	float sunElevationDeg = 55.0f; // above horizon
	glm::vec3 sunColor{ 1.0f, 0.95f, 0.90f };
	float sunIntensity = 10000.0f; // lux-like, matches directional light
	float sunDiscSizeDeg = 1.5f;   // angular radius for the visible disc
	bool sunDiscEnabled = true;

	glm::vec3 fogColor{ 0.65f, 0.75f, 0.90f };
	float fogIntensity = 600.0f; // HDR multiplier; default matches sky horizon
	int fogMode = 0;             // 0=off, 1=exp2, 2=linear
	float fogDensity = 0.01f;    // exp2 mode
	float fogStart = 10.0f;      // linear mode
	float fogEnd = 80.0f;        // linear mode
	float fogHeight = 0.0f;      // height fog reference
	float fogHeightFalloff = 0.0f; // 0 = disabled

	glm::vec3 SunDirectionToSun() const {
		const float az = glm::radians(sunAzimuthDeg);
		const float el = glm::radians(sunElevationDeg);
		const float ce = std::cos(el);
		glm::vec3 d{ std::sin(az) * ce, std::sin(el), std::cos(az) * ce };
		const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		if (len < 1e-6f)
			return glm::vec3(0.0f, 1.0f, 0.0f);
		return d / len;
	}

	EnvironmentFrameData ToFrameData() const {
		EnvironmentFrameData f{};
		f.skyZenith = glm::vec4(skyZenithColor, skyIntensity);
		f.skyHorizon = glm::vec4(skyHorizonColor, skyExponent);
		f.ground = glm::vec4(groundColor, iblEnabled ? iblIntensity : 0.0f);
		const glm::vec3 toSun = SunDirectionToSun();
		float discCos = -1.0f; // disabled sentinel (dot < discCos never true)
		if (sunDiscEnabled) {
			const float r = glm::radians(glm::clamp(sunDiscSizeDeg, 0.05f, 45.0f));
			discCos = std::cos(r);
		}
		f.sunDirection = glm::vec4(toSun, discCos);
		f.sunColor = glm::vec4(sunColor, sunIntensity);
		f.fogColor = glm::vec4(fogColor, fogIntensity);
		f.fogDensityHeight = glm::vec4(fogDensity, fogHeight, fogHeightFalloff, fogStart);
		f.fogEndAndFlags = glm::vec4(
			fogEnd,
			skyEnabled ? kSkyGradient : kSkyOff,
			static_cast<float>(fogMode),
			iblEnabled ? 1.0f : 0.0f);
		return f;
	}
};

inline constexpr const char* kSkyVsPath = "content/shaders/sky.vert.hlsl.spv";
inline constexpr const char* kSkyPsPath = "content/shaders/sky.frag.hlsl.spv";

// Fullscreen-triangle sky program (sky.vert mainVS + sky.frag mainPS).
// Reads FrameUniforms via the shared PushConstants (frameUniforms pointer),
// so the app just pushes its usual PC before drawing.
struct Skydome {
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> fragModule;
	ShaderProgram program{};

	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kSkyVsPath,
		const char* psPath = kSkyPsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kSkyVsPath;
			const std::string ps = psPath ? psPath : kSkyPsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "skyVert");
			if (fragModule.empty())
				fragModule = ctx.CreateShaderModule(readFile(ps), "skyFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || fragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader ps{ .module = fragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		program = { .shaders = { vs, ps }, .shaderCount = 2 };
		return true;
	}

	bool Ensure(ResourceManager& ctx,
		const char* vsPath = kSkyVsPath,
		const char* psPath = kSkyPsPath) {
		return EnsureShaders(ctx, vsPath, psPath);
	}

	void DestroyShaders() {
		program = ShaderProgram{};
		vsModule = {};
		fragModule = {};
	}
};

} // namespace smvk
