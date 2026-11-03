// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/GI/bake.h>
#include <Engine/Renderer/ReferenceSceneRenderer.h>
#include <atomic>
#include <cassert>
#include <cmath>
#include <stdexcept>

int main() {
	using namespace tour;
	gi::GiTraceScene scene;
	scene.positions = {{-1.0f, -1.0f, 0.0f},
	                   { 1.0f, -1.0f, 0.0f},
	                   { 0.0f,  1.0f, 0.0f}};
	scene.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
	scene.indices = {0, 1, 2};
	scene.triMaterial = {0};
	gi::GiTraceMaterial mat{};
	mat.albedo = {0, 0, 0};
	mat.baseColor = {0, 0, 0};
	mat.emissive = {8, 2, 1};
	scene.materials.push_back(mat);
	scene.skyIntensity = 0;
	scene.sunIntensity = 0;
	scene.SetTraceBackend(gi::TraceBackend::Embree);
	scene.Finalize();
	assert(scene.Bvh().Backend() == gi::TraceBackend::Embree);

	renderer::ReferenceCamera camera{};
	camera.position = {0, 0, 2};
	camera.forward = {0, 0, -1};
	renderer::ReferenceRenderSettings opts{};
	opts.width = 32;
	opts.height = 32;
	opts.samplesPerPixel = 8;
	opts.maxBounces = 0;
	opts.threads = 1;
	std::atomic<uint32_t> progress{0};
	auto first = renderer::ReferenceSceneRenderer::Render(
		scene, camera, opts, nullptr, &progress);
	assert(progress.load() == opts.height);
	assert(first.linearRadiance.size() == size_t(opts.width) * opts.height);
	assert(first.linearRadiance[16 * opts.width + 16].r > 7.9f);
	assert(first.linearRadiance[16 * opts.width + 16].g > 1.9f);
	const auto ldr = first.ToRgba8(0.1f);
	assert(ldr.size() == size_t(opts.width) * opts.height * 4);
	assert(ldr[(16 * opts.width + 16) * 4 + 3] == 255);

	opts.threads = 4;
	const auto multi = renderer::ReferenceSceneRenderer::Render(scene, camera, opts);
	assert(first.linearRadiance.size() == multi.linearRadiance.size());
	for (size_t i = 0; i < first.linearRadiance.size(); ++i) {
		const auto& a = first.linearRadiance[i];
		const auto& b = multi.linearRadiance[i];
		assert(a.x == b.x && a.y == b.y && a.z == b.z);
	}
	opts.threads = 1;
	camera.orthoHalfHeight = 1.0f;
	const auto ortho = renderer::ReferenceSceneRenderer::Render(scene, camera, opts);
	assert(ortho.linearRadiance[16 * opts.width + 16].r > 7.9f);
	std::atomic_bool cancelled{true};
	assert(renderer::ReferenceSceneRenderer::Render(scene, camera, opts,
		&cancelled).linearRadiance.empty());
	opts.width = 0;
	bool rejected = false;
	try {
		(void)renderer::ReferenceSceneRenderer::Render(scene, camera, opts);
	}
	catch (const std::invalid_argument&) {
		rejected = true;
	}
	assert(rejected);
	return 0;
}
