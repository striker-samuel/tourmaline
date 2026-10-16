// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// PushConstantLayoutTests: pins the IRenderer::PushConstants mirror.
// The C++ struct (Engine/Renderer/Renderer.h) and the HLSL struct
// (engine/shaders/push_constants.hlsli) must stay layout-identical or the
// GPU reads shifted fields (the 10/1 device-loss class). The static_asserts
// in Renderer.h catch C++-side drift at compile time; these runtime checks
// additionally verify size/alignment/contiguity and that every field the
// YAML IDL declares is covered by an offsetof assert (via codegen.py).
//
// Run: ctest -R push_constant_tests. CI also runs
//   python tools/push_constants_idl/codegen.py --check
// which parses both sides and fails on drift.

#include <cstddef>
#include <cstdint>
#include <iostream>

#include <Engine/Renderer/Renderer.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	if (++g_checks, cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

} // namespace

int main() {
	using PC = engine::IRenderer::PushConstants;
	Check(sizeof(PC) == 256, "push.size-256");
	Check(alignof(PC) == 16, "push.align-16");
	Check(offsetof(PC, model) == 0, "push.model-0");
	Check(offsetof(PC, frameUniforms) == 64, "push.frameUniforms-64");
	Check(offsetof(PC, lightsBuffer) == 72, "push.lightsBuffer-72");
	Check(offsetof(PC, materialsBuffer) == 80, "push.materialsBuffer-80");
	Check(offsetof(PC, cascadeBuffer) == 88, "push.cascadeBuffer-88");
	Check(offsetof(PC, materialId) == 96, "push.materialId-96");
	Check(offsetof(PC, comparisonSamplerId) == 100, "push.comparisonSamplerId-100");
	Check(offsetof(PC, numCascades) == 104, "push.numCascades-104");
	Check(offsetof(PC, cascadeIndex) == 108, "push.cascadeIndex-108");
	Check(offsetof(PC, fpLights) == 112, "push.fpLights-112");
	Check(offsetof(PC, fpTileCounts) == 120, "push.fpTileCounts-120");
	Check(offsetof(PC, fpTileIndices) == 128, "push.fpTileIndices-128");
	Check(offsetof(PC, fpTileCountX) == 136, "push.fpTileCountX-136");
	Check(offsetof(PC, fpLightCount) == 140, "push.fpLightCount-140");
	Check(offsetof(PC, fpTileSize) == 144, "push.fpTileSize-144");
	Check(offsetof(PC, fpMaxPerTile) == 148, "push.fpMaxPerTile-148");
	Check(offsetof(PC, spotShadowBuffer) == 152, "push.spotShadowBuffer-152");
	Check(offsetof(PC, spotShadowMapIndex) == 160, "push.spotShadowMapIndex-160");
	Check(offsetof(PC, spotShadowCount) == 164, "push.spotShadowCount-164");
	Check(offsetof(PC, spotIndex) == 168, "push.spotIndex-168");
	Check(offsetof(PC, pad_pc) == 172, "push.pad_pc-172");
	Check(offsetof(PC, renderViews) == 176, "push.renderViews-176");
	Check(offsetof(PC, mainRenderView) == 184, "push.mainRenderView-184");
	Check(offsetof(PC, renderViewIndex) == 188, "push.renderViewIndex-188");
	Check(offsetof(PC, prevModel) == 192, "push.prevModel-192");
	Check(sizeof(PC::prevModel) == 48, "push.prevModel-48B");
	Check(offsetof(PC, padEnd_) == 240, "push.padEnd-240");
	// Scratch rule: shared fields end at 192, overlay owns 192..256.
	Check(offsetof(PC, prevModel) == 192 && sizeof(PC) - 192 == 64,
		"push.scratch-64B-tail");
	// Contiguity: no hidden padding between consecutive shared fields
	// (pad_pc is the intentional transparency-technique word, padEnd_ the tail).
	Check(offsetof(PC, comparisonSamplerId) == offsetof(PC, materialId) + 4,
		"push.packed-material-words");
	Check(offsetof(PC, fpLightCount) == offsetof(PC, fpTileCountX) + 4,
		"push.packed-fp-words");
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
