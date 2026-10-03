// Pak-viewer host run: renderer setup + world source + main loop.
// Moved verbatim from the old App/main.cpp (host logic now lives in the
// engine); the only delta is app:: -> smvk:: after the renderer move.

#include <Engine/App/Host.h>
#include "PakPreload.h"

#include <Engine/Memory/memory.h>
#include <Engine/Profile/profile.h>

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Flintbase/flintbase_backend.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <Engine/Vfs/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace engine {
namespace {

// Pak stem -> scene id (GI sidecars resolve gi/<id>.*, check-scene
// extras key on it; see WorldRenderer::SetSceneId).
std::string StemOf(const std::string& p) {
	const size_t slash = p.find_last_of("/\\");
	const std::string file = (slash == std::string::npos)
		? p : p.substr(slash + 1);
	const size_t dot = file.find_last_of('.');
	return (dot == std::string::npos || dot == 0)
		? file : file.substr(0, dot);
}

// UV sphere (lat-long) centered on the origin — the sponza check
// scene's chrome probe ball. Triangles follow the (a,b,c),(a,c,d)
// pattern with b one step in +theta (longitude) and d one step in
// +phi (pole to pole): dTheta x dPhi points outward, so the winding
// matches the engine's CCW front faces.
smvk::StaticMeshUpload BuildChromeBallUpload(float radius, int slices, int stacks) {
	smvk::StaticMeshUpload data;
	data.aabbMin = glm::vec3(-radius);
	data.aabbMax = glm::vec3(radius);
	const float pi = glm::pi<float>();
	const int nSlices = std::max(8, slices);
	const int nStacks = std::max(4, stacks);
	for (int j = 0; j <= nStacks; ++j) {
		const float phi = pi * static_cast<float>(j) / static_cast<float>(nStacks);
		const float sinPhi = std::sin(phi);
		const float cosPhi = std::cos(phi);
		for (int i = 0; i <= nSlices; ++i) {
			const float theta = 2.0f * pi * static_cast<float>(i) / static_cast<float>(nSlices);
			const float sinT = std::sin(theta);
			const float cosT = std::cos(theta);
			const glm::vec3 n(sinPhi * cosT, cosPhi, sinPhi * sinT);
			smvk::StaticVertex v{};
			v.pos = n * radius;
			v.color = glm::vec4(1.0f);
			v.texCoord = glm::vec2(
				static_cast<float>(i) / static_cast<float>(nSlices),
				static_cast<float>(j) / static_cast<float>(nStacks));
			v.normal = n;
			glm::vec3 t(-sinT, 0.0f, cosT);
			if (glm::dot(t, t) < 1e-12f)
				t = glm::vec3(1.0f, 0.0f, 0.0f); // poles: theta tangent degenerates
			v.tangent = glm::vec4(glm::normalize(t), 1.0f);
			data.vertices.push_back(v);
		}
	}
	const uint32_t row = static_cast<uint32_t>(nSlices) + 1u;
	for (int j = 0; j < nStacks; ++j) {
		for (int i = 0; i < nSlices; ++i) {
			const uint32_t a = static_cast<uint32_t>(j) * row + static_cast<uint32_t>(i);
			const uint32_t b = a + 1u;
			const uint32_t c = a + row + 1u;
			const uint32_t d = a + row;
			data.indices.push_back(a);
			data.indices.push_back(b);
			data.indices.push_back(c);
			data.indices.push_back(a);
			data.indices.push_back(c);
			data.indices.push_back(d);
		}
	}
	return data;
}

// Chrome probe ball (sponza check scene only): procedural UV sphere,
// 0.5 m in diameter, at the atrium center resting on the floor (y=0).
// White metal (metallic 1, roughness 0; the lit shader clamps
// roughness to 0.04 so there are no GGX singularities). Injected into
// the loaded cluster BEFORE Resolve so it joins culling, shadows, AO
// and reflections like any imported node. Independent root (own
// identity-parent transform), so scene root transforms can't move it
// and the static-freeze loop below simply leaves it dynamic.
void InjectSponzaChromeBall(smvk::World& world, smvk::ResourceManager& ctx) {
	(void)ctx;
	if (world.clusters.empty())
		return;
	smvk::Cluster& sponzaCluster = *world.clusters.back();
	const float ballRadius = 0.25f;
	const glm::vec3 ballCenter(0.0f, ballRadius, 0.0f);
	smvk::StaticMeshUpload ballUpload = BuildChromeBallUpload(ballRadius, 48, 32);
	smvk::ClusterStaticMeshHandle ballMesh = sponzaCluster.UploadStaticMesh(ballUpload);
	smvk::MaterialCreateInfo chromeCi{};
	chromeCi.baseColorFactor = glm::vec4(1.0f);
	chromeCi.metallicFactor = 1.0f;
	chromeCi.roughnessFactor = .13f;
	chromeCi.emissiveFactor = glm::vec3(0.0f);
	chromeCi.alphaMode = smvk::AlphaMode::Opaque;
	chromeCi.shadingModel = smvk::ShadingModel::DefaultLit;
	smvk::ClusterMaterialHandle chromeMat = sponzaCluster.CreateMaterial(chromeCi);
	if (ballMesh.empty() || chromeMat.empty()) {
		std::cerr << "[Scene] chrome ball inject failed (mesh/material)\n";
		return;
	}
	const int ballNode = addNode(sponzaCluster.scene, -1, 0);
	sponzaCluster.scene.localTransform[ballNode] =
		glm::translate(glm::mat4(1.0f), ballCenter);
	sponzaCluster.scene.globalTransform[ballNode] =
		glm::translate(glm::mat4(1.0f), ballCenter);
	sponzaCluster.scene.meshForNode[ballNode] = ballMesh.index();
	sponzaCluster.scene.materialForNode[ballNode] = chromeMat.index();
	setNodeName(sponzaCluster.scene, ballNode, "chrome_ball");
	markAsChanged(sponzaCluster.scene, ballNode);
	std::cerr << "[Scene] chrome ball: r=" << ballRadius
		<< "m at (" << ballCenter.x << "," << ballCenter.y << "," << ballCenter.z << "), "
		<< ballUpload.vertices.size() << " verts, "
		<< ballUpload.indices.size() / 3 << " tris\n";
}

// The HOST's pak loader (the renderer's world source): consumes the
// preloaded backend (read + parse + decompress already overlapped with
// renderer init), loads the selected scene into a smvk::World (uploads issued
// to the context — everything renders by handles), injects the
// check-scene extras, freezes the cooked roots static and resolves.
// The pak's embedded textures mount into the renderer's content
// overlay (below loose content/, so dev overrides win) for init-time
// reads (cubemaps baked into the pak, ...). Returns nullptr on any
// failure (the renderer fails loudly).
smvk::mem::LinearPtr<smvk::World> LoadPakWorld(PakPreload pre,
	int sceneIndex, smvk::ResourceManager& ctx, smvk::OverlayVfs& contentVfs,
	smvk::TextureStreamer* sceneStreamer) {
	if (!pre.backend) {
		std::cerr << "[App] pak preload failed: " << pre.error << "\n";
		return nullptr;
	}
	std::cerr << "[App] loading pak '" << pre.scenePak << "'\n";
	const smvk::PakReader& reader = pre.backend->Reader();
	std::cerr << "[App] pak parsed (" << reader.EntryCount() << " entries)\n";
	// Scene discovery: list, default to index 0.
	std::string lerr;
	auto scenes = smvk::ListPakScenes(reader, &lerr);
	if (scenes.empty()) {
		std::cerr << "[App] no loadable scenes: " << lerr << "\n";
		return nullptr;
	}
	for (size_t i = 0; i < scenes.size(); ++i)
		std::cerr << "[App]   scene[" << i << "] '" << scenes[i].name
			<< "' nodes=" << scenes[i].nodes << "\n";
	if (sceneIndex < 0 || size_t(sceneIndex) >= scenes.size()) {
		std::cerr << "[App] scene index " << sceneIndex << " out of range ("
			<< scenes.size() << " scenes)\n";
		return nullptr;
	}
	std::cerr << "[App] loading scene[" << sceneIndex << "] '"
		<< scenes[size_t(sceneIndex)].name << "'...\n";
	auto world = smvk::mem::MakeProcess<smvk::World>();
	try {
		world->LoadClusterFromPak(ctx, reader, uint32_t(sceneIndex),
			sceneStreamer, pre.image);
	}
	catch (const std::exception& e) {
		// DeserializeCluster drains its own staged uploads on failure;
		// drain anything queued since, then fail the load.
		std::cerr << "[App] pak load failed: " << e.what() << "\n";
		ctx.WaitForUploads();
		return nullptr;
	}
	std::cerr << "[App] pak cluster loaded\n";
	// Check-scene extras (sponza chrome ball) inject BEFORE resolve so
	// they join culling/shadows/AO/reflections like any imported node.
	if (StemOf(pre.scenePak) == "sponza")
		InjectSponzaChromeBall(*world, ctx);
	// Freeze everything static (no recook needed): the exporters stamp
	// no STATIC_ marks, so cooked paks carry staticRoots=0 and the
	// smvk::World's static BVH would stay empty. These scenes never animate,
	// so mark each cluster root static here; Resolve carries the spec
	// into the world scene and RebuildStaticBvh picks it up. Future
	// dynamic nodes must ClearSubtreeStatic() before Resolve to opt out.
	for (auto& clusterPtr : world->clusters) {
		if (clusterPtr && !clusterPtr->scene.hierarchy.empty())
			MarkSubtreeStatic(clusterPtr->scene, 0);
	}
	world->ResolveAndFixup_clusters();
	// Serve pak-embedded textures through the renderer's content
	// overlay (below loose content/, so dev overrides win).
	contentVfs.Mount(pre.backend, 10, pre.scenePak);
	return world;
}

// Bisection: layer extra passes onto the base config
// (csv: deferred,shadows,ssao/gtao/ao,smaa,motion,dof,ssr,bloom,exposure,msaa,skydome,ui,cull,particles).
void ApplyPassCsv(ScenePassConfig& passCfg, const char* csv, bool enable) {
	if (!csv || !*csv) return;
	const std::string s = csv;
	size_t pos = 0;
	while (pos <= s.size()) {
		size_t end = s.find(',', pos);
		if (end == std::string::npos) end = s.size();
		const std::string tok = s.substr(pos, end - pos);
		bool* flag = nullptr;
		if (tok == "deferred") flag = &passCfg.deferredShadows;
		else if (tok == "shadows") { passCfg.directionalShadows = passCfg.spotShadows = enable; }
		else if (tok == "ssao" || tok == "gtao" || tok == "ao") flag = &passCfg.ssao;
		else if (tok == "smaa") flag = &passCfg.smaa;
		else if (tok == "motion") { passCfg.motionVectors = passCfg.motionBlur = enable; }
		else if (tok == "dof") flag = &passCfg.dof;
		else if (tok == "ssr") flag = &passCfg.ssr;
		else if (tok == "bloom") flag = &passCfg.bloom;
		else if (tok == "exposure") flag = &passCfg.exposure;
		else if (tok == "msaa") flag = &passCfg.msaa;
		else if (tok == "skydome") flag = &passCfg.skydome;
		else if (tok == "ui") flag = &passCfg.ui;
		else if (tok == "cull") flag = &passCfg.forwardPlusCull;
		else if (tok == "prepass") flag = &passCfg.forwardPlusPrepass;
		else if (tok == "filter") flag = &passCfg.gfxFilter;
		else if (tok == "particles") flag = &passCfg.particles;
		else if (!tok.empty()) std::cerr << "Unknown pass token: '" << tok << "'\n";
		if (flag) *flag = enable;
		pos = end + 1;
	}
}

} // namespace

int RunPakApp(PakAppConfig cfg) {
	// Reserve linear-allocator virtual address space FIRST, before worker
	// threads, Vulkan, or asset loading (see Engine/Memory/memory.h). An
	// unsatisfiable reservation fails fast here with the OOM diagnostic.
	smvk::mem::InitMemory();

	// Entrypoint: a pak file + a scene index (0 = default). The scene
	// id (GI sidecar stem + check-scene gating) is the pak stem.
	std::string sceneId = cfg.sceneId.empty() ? StemOf(cfg.pakPath) : cfg.sceneId;
	if (sceneId.empty())
		sceneId = "pak";
	std::cerr << "Scene: pak '" << cfg.pakPath << "' (id '" << sceneId
		<< "', index " << cfg.sceneIndex << ")\n";

	// Pak preload: kick the file read + parse + decompress NOW, before
	// the renderer starts, so it overlaps Vulkan init + shader loading
	// (the old flow did all of it inside Initialize, after the GPU and
	// every shader were already up). The future rides a shared_ptr so
	// the copyable WorldSource can hold it until the renderer calls.
	auto preloadFuture = std::make_shared<std::future<PakPreload>>(
		KickPakPreload(cfg.pakPath));

	WorldRenderer renderer;
	renderer.SetSceneId(sceneId);
	// The HOST owns scene loading: the renderer calls this once during
	// Initialize, after its context exists (nothing can upload before
	// that), and renders the returned world by handles. By then the
	// preload has typically been running for the whole of renderer init.
	// The renderer's texture streamer rides along so v4 digest textures
	// register for demand upgrades (null when streaming is off → full
	// resident, the pre-v4 behavior). The preloaded image pins the
	// borrowed tail payloads.
	const int sceneIndex = cfg.sceneIndex;
	renderer.SetWorldSource(
		[preloadFuture, sceneIndex, &renderer](smvk::ResourceManager& ctx,
			smvk::OverlayVfs& contentVfs) -> smvk::mem::LinearPtr<smvk::World> {
			return LoadPakWorld(preloadFuture->get(), sceneIndex,
				ctx, contentVfs, renderer.GetTextureStreamer());
		});
	ScenePassConfig passCfg = ScenePassConfig::All();
	if (cfg.filterOnly)
		passCfg = ScenePassConfig::FilterOnly();
	else if (cfg.minimal)
		passCfg = ScenePassConfig::Minimal();
	ApplyPassCsv(passCfg, cfg.withCsv.c_str(), true);
	ApplyPassCsv(passCfg, cfg.withoutCsv.c_str(), false);
	renderer.Configure(passCfg);
	// --no-dlss: suppress the Streamline load BEFORE the renderer runs
	// (App::Initialize consults the flag before DlssStartup /
	// pointing volk at the interposer — after that the decision is locked
	// in) and force the mode off so no feature evaluation happens either.
	// This is the RenderDoc-safe configuration.
	if (cfg.noDlss) {
		smvk::DlssSetStartupEnabled(false);
		renderer.SetDlssMode(smvk::DlssMode::Off);
		std::cerr << "[DLSS] disabled (--no-dlss): Streamline interposer not loaded\n";
	}
	if (cfg.disableControl) {
		renderer.SetDisableControl(true);
		std::cerr << "[Scene] camera control disabled (--disable_control)\n";
	}
	if (!cfg.presentMode.empty()) {
		if (cfg.presentMode == "fifo")
			renderer.SetPresentMode(smvk::PresentMode::Fifo);
		else if (cfg.presentMode == "mailbox")
			renderer.SetPresentMode(smvk::PresentMode::Mailbox);
		else if (cfg.presentMode == "immediate")
			renderer.SetPresentMode(smvk::PresentMode::Immediate);
		else {
			std::cerr << "Unknown present mode: '" << cfg.presentMode << "' (fifo|mailbox|immediate)\n";
			return EXIT_FAILURE;
		}
	}
	int benchFrames = cfg.benchFrames;
	if (benchFrames > 0) {
		renderer.SetFrameBudget(benchFrames);
		// Bench captures must be pixel-comparable across runs: drop eye
		// adaptation and pin the fixed manual exposure (X re-enables live).
		renderer.SetAutoExposureEnabled(false);
		std::cout << "bench: auto-exposure off (fixed manual exposure)\n";
	}
	if (cfg.benchMoveSpeed > 0.0f) {
		// Constant-velocity flight needs --bench to be meaningful (the
		// movement gates on the measured window); pair them up here so a
		// bare --bench_move still does the right thing.
		if (benchFrames <= 0) {
			benchFrames = 300;
			renderer.SetFrameBudget(benchFrames);
			renderer.SetAutoExposureEnabled(false);
			std::cout << "bench: auto-exposure off (fixed manual exposure)\n";
		}
		renderer.SetBenchMoveSpeed(cfg.benchMoveSpeed);
		std::cout << "bench: constant-velocity flight " << cfg.benchMoveSpeed
			<< " m/s forward from the spawn pose (past warmup)\n";
	}
	// Optick gates must be set before Run(): the engine initializes the GPU
	// profiler during VulkanCore init, and the bench session starts after the
	// warmup frames. --use_optick_gpu implies --use_optick.
	smvk::profiler::SetEnabled(cfg.useOptick);
	if (cfg.useOptickGpu)
		smvk::profiler::SetGpuEnabled(true);
	try {
		renderer.Run();
	}
	catch (const std::exception& e) {
		std::cerr << "\nERROR: " << e.what() << std::endl;
		std::cerr << "\nPress Enter to exit...";
		std::cin.get();
		return EXIT_FAILURE;
	}
	smvk::mem::ShutdownMemory(); // footprint report (used/committed/reserved)
	return EXIT_SUCCESS;
}

int RunPakApp(int argc, const char* argv[]) {
	PakAppConfig cfg;
	int exitCode = EXIT_SUCCESS;
	if (!ParsePakAppArgs(argc, argv, cfg, exitCode))
		return exitCode;
	return RunPakApp(cfg);
}

} // namespace engine
