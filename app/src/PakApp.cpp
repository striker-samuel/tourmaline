// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Pak-viewer host run: renderer setup + world source + main loop. This is
// the APP's host layer (moved out of the engine — the engine renders
// worlds, the app owns pak loading, check-scene extras and demo content).
// Identifiers: smvk:: = engine runtime, engine:: = the renderer layer.

#include "Host.h"
#include "PakPreload.h"

#include <Engine/Memory/memory.h>
#include <Engine/Profile/profile.h>

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Renderer/WorldRenderer.h>
#include <Engine/Renderer/WorldRendererElement.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <Engine/Vfs/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

namespace app {
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

		// (The sponza CHECK-SCENE EXTRAS were REMOVED — the chrome probe
		// ball and the knight companion + its ground quad: the Asteroids
		// game owns the atrium now and the plain pak content is the whole
		// scene. The ball recipe (procedural UV sphere + PBR material)
		// lives on in asteroids/src/InvadersGame.cpp.)

		// The HOST's pak loader (the renderer's world source): consumes the
		// preloaded backend (read + parse + decompress already overlapped with
		// renderer init), loads the selected scene into a smvk::World (uploads
		// issued to the context — everything renders by handles), freezes the
		// cooked roots static and resolves.
		smvk::mem::LinearPtr<smvk::World> LoadPakWorld(
			PakPreload pre,
			int sceneIndex,
			smvk::ResourceManager& ctx,
			smvk::OverlayVfs& contentVfs,
			smvk::TextureStreamer* sceneStreamer
		) {
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
				// Terrain-only pak (no scenes, Terrain blobs present): an empty
				// world carrying just the harvested terrain. The renderer bakes
				// G-blocks from it; the camera needs explicit TOURMALINE_CAM_*
				// pins (empty-scene framing has no bounds to fit).
				if (!reader.BlobsOfKind(smvk::PakAssetKind::Terrain).empty()) {
					std::cerr << "[App] no scenes: terrain-only pak, loading "
						"terrain without a scene\n";
					auto world = smvk::mem::MakeProcess<smvk::World>();
					std::string terrErr;
					if (!world->LoadTerrainFromPak(reader, terrErr)) {
						std::cerr << "[App] terrain harvest failed: " << terrErr
							<< "\n";
						return nullptr;
					}
					world->ResolveAndFixup_clusters();
					contentVfs.Mount(pre.backend, 10, pre.scenePak);
					return world;
				}
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
			// THE SKINNING CLIP (the Northlight bone-eval box's input): a loaded
			// .tourAnim clip attaches to the first cluster carrying a skeleton (the
			// knight pak's cluster). Sources, first match wins:
			//   1. TOURMALINE_ANIM=<vfs path> (the explicit headless pin),
			//   2. <scene pak stem>.anim* beside the pak (content/demo-style
			//      layouts where the .anim0_* asset sits next to the scene).
			// No clip = the bind-pose palette (the pass still runs, nothing
			// moves - the deformed heap exists, the fetch swap works).
			// Attaches the first loadable '<stem>.anim*' clip to a skinned cluster.
			auto attachClip = [&](smvk::Cluster& c, const std::string& stem) {
				if (c.HasAnimClip())
					return;
				std::vector<std::string> candidates;
				if (const char* env = std::getenv("TOURMALINE_ANIM"))
					candidates.push_back(env);
				{
					// The importer names clips
					// '<stem>.anim<idx>_<sanitized glTF animation name>.anim',
					// so enumerate the demo dir for '<stem>.anim*' rather than
					// guessing a label. ListDir is sorted, so this is stable.
					const std::string prefix = "demo/" + stem + ".anim";
					for (const smvk::VfsPath& e : contentVfs.ListDir("demo")) {
						if (e.size() >= prefix.size() + 5
							&& e.compare(0, prefix.size(), prefix) == 0
							&& e.compare(e.size() - 5, 5, ".anim") == 0)
							candidates.push_back(e);
					}
				}
				for (const std::string& path : candidates) {
					const std::string norm = smvk::NormalizeAssetUri(path).Str();
					auto bytes = contentVfs.TryRead(norm.empty() ? path : norm);
					if (!bytes || bytes->empty())
						continue;
					smvk::touranim::AnimDoc doc;
					std::string aerr;
					if (!smvk::touranim::LoadTourAnimText(
						std::string(bytes->begin(), bytes->end()), doc, aerr)) {
						std::cerr << "[App] anim load failed: " << aerr
							<< " (bind-pose palette)\n";
						return;
					}
					if (doc.clips.empty())
						return;
					// Print BEFORE the move (the log would otherwise report the
					// moved-from clip: '' + 0 tracks).
					std::cerr << "[App] skinning clip attached: '"
						<< doc.clips.front().name << "' ("
						<< doc.clips.front().tracks.size() << " tracks, "
						<< doc.clips.front().duration << " s)\n";
					c.SetAnimClip(std::move(doc.clips.front()));
					return;
				}
			};
			// (No check-scene extras anymore — the loaded pak content is the
			// whole scene; see the removal note above.)
			for (auto& clusterPtr : world->clusters) {
				smvk::Cluster* c = clusterPtr.get();
				if (!c || !c->hasSkeleton)
					continue;
				attachClip(*c, StemOf(pre.scenePak));
				break; // ONE cluster with a skeleton for now (the driver's cap)
			}
			// Freeze everything static (no recook needed): the exporters stamp
			// no STATIC_ marks, so cooked paks carry staticRoots=0 and the
			// smvk::World's static BVH would stay empty. These scenes never animate,
			// so mark each cluster root static here; Resolve carries the spec
			// into the world scene and RebuildStaticBvh picks it up. Skinned
			// clusters are the exception: their mesh nodes carry a live placement
			// and the joint chains move every frame, so they stay dynamic (the
			// animation driver owns their transforms).
			for (auto& clusterPtr : world->clusters) {
				if (clusterPtr && !clusterPtr->hasSkeleton
					&& !clusterPtr->scene.hierarchy.empty())
					MarkSubtreeStatic(clusterPtr->scene, 0);
			}
			world->ResolveAndFixup_clusters();
			// SSR wax (test amplification, TOURMALINE_SSR_WAX=1; the sponza
			// check scene is the target): every opaque lit material's roughness
			// -> 0.04 (the shader-side clamp floor - a dielectric mirror). The
			// sponza floor's cooked roughness leaves a legitimate but FAINT
			// dielectric reflection lobe (grazing F0 ~0.04) that is easy to
			// misread as "SSR contributes nothing"; the wax makes every
			// receiver - the floor especially, its grazing-angle pillar
			// reflections - a mirror so the trace's hit/miss + the compose are
			// plainly visible (the removed chrome probe ball used to be the
			// calibrated reference; the waxed floor carries that role now).
			// AFTER ResolveAndFixup (the material location table is the resolve's
			// product); the mutations flow to the GPU per frame through
			// UpdateMaterials' world reads.
			if (const char* waxEnv = std::getenv("TOURMALINE_SSR_WAX");
				waxEnv && *waxEnv && *waxEnv != '0') {
				uint32_t waxed = 0, total = 0;
				for (uint32_t mi = 0;; ++mi) {
					smvk::Material* m = world->GetMaterial(mi);
					if (!m)
						break;
					++total;
					if (m->shadingModel != smvk::ShadingModel::DefaultLit
						|| m->alphaMode != smvk::AlphaMode::Opaque)
						continue;
					m->values.params.z = 0.04f; // roughness -> the clamp floor
					++waxed;
				}
				std::cerr << "[Scene] SSR wax: " << waxed << "/" << total
					<< " opaque lit materials -> roughness 0.04 (mirror)\n";
			}
			if (std::getenv("TOURMALINE_KD")) {
				int n = 0;
				for (auto& [node, meshIdx] : world->scene.meshForNode) {
					if (meshIdx >= world->meshLocations.size())
						continue;
					smvk::Cluster* cl = world->meshLocations[meshIdx].cluster;
					if (!cl || !cl->hasSkeleton)
						continue;
					const glm::mat4& g = world->scene.globalTransform[node];
					std::cerr << "[KD] merged meshNode=" << node
						<< " t=(" << g[3].x << "," << g[3].y << "," << g[3].z
						<< ") sc=(" << glm::length(glm::vec3(g[0])) << ","
						<< glm::length(glm::vec3(g[1])) << ","
						<< glm::length(glm::vec3(g[2])) << ")\n";
					if (++n > 8) break;
				}
				std::cerr << "[KD] skinned draw nodes: " << n << "\n";
			}
			// (Demo decal seeding — the procedural blood textures + sponza hero
			// splats — is gone with DecalAssets.cpp: decals resolve from world
			// content only now. The renderer's own TOURMALINE_DECALS visibility
			// toggle is untouched.)
			// Serve pak-embedded textures through the renderer's content
			// overlay (below loose content/, so dev overrides win).
			contentVfs.Mount(pre.backend, 10, pre.scenePak);
			return world;
		}

		// Bisection: layer extra passes onto the base config
		// (csv: deferred,shadows,ssao/gtao/ao,smaa,motion,dof,ssr,bloom,exposure,msaa,skydome,ui,cull,particles).
		void ApplyPassCsv(engine::WorldRendererConfig& passCfg, const char* csv, bool enable) {
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
				else if (tok == "terrain") flag = &passCfg.terrain;
				else if (tok == "ui") flag = &passCfg.ui;
				else if (tok == "cull") flag = &passCfg.forwardPlusCull;
				else if (tok == "prepass") flag = &passCfg.forwardPlusPrepass;
				else if (tok == "filter") flag = &passCfg.gfxFilter;
				else if (tok == "particles") flag = &passCfg.particles;
				// Memory aliasing (the target-heap master gate - see
				// smvk::TargetHeapAliasingEnabled): not a pass bool - the token
				// maps to the global gate directly. --without memory_aliasing
				// (default state) keeps every packed range disjoint; --with
				// memory_aliasing honors the declared windows. Composes with the
				// env pin (TOURMALINE_ALIASING): the CLI token applies here, at
				// attach time, BEFORE any target claims - so it wins.
				else if (tok == "memory_aliasing" || tok == "aliasing")
					smvk::SetTargetHeapAliasing(enable);
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
		// the app boots, so it overlaps Vulkan init + shader loading. The
		// future rides a shared_ptr so the copyable WorldSource can hold it
		// until the renderer calls.
		auto preloadFuture = std::make_shared<std::future<PakPreload>>(
			KickPakPreload(cfg.pakPath));

		// App host + WorldRenderer ELEMENT (nvapp-style: the host owns the
		// window/context/swapchain + the frame sync — the default DrawFrame
		// pump drives the element between its acquire and present; the
		// renderer never acquires/presents on its own).
		smvk::App app;
		// The ELEMENT owns the control layer (input, keybinds, camera drive,
		// UI mode, lifecycle) and wraps the pure frame renderer (see
		// WorldRendererElement.h). Host config drives the renderer through
		// the element's Renderer() directly; control (disable_control /
		// bench_move) goes through the element.
		auto element = std::make_shared<engine::WorldRendererElement>(app);
		engine::WorldRenderer& renderer = element->Renderer();
		renderer.SetSceneId(sceneId);
		// --no-vfs: mute the loose VFS discovery mounts (project root +
		// content/) so the pak backend mounted at load is the only
		// scene-content source (proves the pak is self-contained).
		if (cfg.noVfs) {
			renderer.SetNoVfs(true);
			std::cerr << "[Scene] --no-vfs: only pak '" << cfg.pakPath
				<< "' serves scene content\n";
		}
		// The HOST owns scene loading: the renderer calls this once during
		// onAttach, after the host's context exists (nothing can upload
		// before that), and renders the returned world by handles. By then
		// the preload has typically been running for the whole of app boot.
		// The renderer's texture streamer rides along so v4 digest textures
		// register for demand upgrades (null when streaming is off → full
		// resident, the pre-v4 behavior). The preloaded image pins the
		// borrowed tail payloads. The lambda captures the RAW renderer
		// pointer (it runs once INSIDE the element's onAttach, where the
		// element is alive by definition) — a shared_ptr capture would
		// self-cycle.
		const int sceneIndex = cfg.sceneIndex;
		engine::WorldRenderer* rendererRaw = &renderer;
		// World handoff for the invaders element (app/src/InvadersGame):
		// the lambda stashes the raw world pointer as it returns the
		// LinearPtr to the renderer. The renderer owns the lifetime; the
		// raw pointer stays valid for the game element's whole run (the
		// element stack detaches in reverse order, so the game drops the
		// hook + stops touching the world before the renderer tears it
		// down).
		auto worldHandle = std::make_shared<smvk::World*>();
		renderer.SetWorldSource(
			[preloadFuture, sceneIndex, rendererRaw, worldHandle]
			(
				smvk::ResourceManager& ctx,
				smvk::OverlayVfs& contentVfs
				) -> smvk::mem::LinearPtr<smvk::World> {
			auto world = LoadPakWorld(preloadFuture->get(),
				sceneIndex,
				ctx,
				contentVfs,
				rendererRaw->GetTextureStreamer()
				);
			*worldHandle = world.get();
			return world;
		});
		engine::WorldRendererConfig passCfg = engine::WorldRendererConfig::All();
		if (cfg.filterOnly)
			passCfg = engine::WorldRendererConfig::FilterOnly();
		else if (cfg.minimal)
			passCfg = engine::WorldRendererConfig::Minimal();
		ApplyPassCsv(passCfg, cfg.withCsv.c_str(), true);
		ApplyPassCsv(passCfg, cfg.withoutCsv.c_str(), false);
		renderer.Configure(passCfg, cfg.run);
		// (Demo content seeding — embers at the origin + decal presets —
		// REMOVED: the hosts run the plain pak scene now. The renderer never
		// seeds content on its own; its particle system stays fully live for
		// panel-authored emitters and the Asteroids game's bursts
		// (WorldRenderer::SpawnParticleBurst).)
		// --no-dlss / --capture (the latter implies the former): suppress the
		// Streamline load BEFORE the app runs (App::Initialize consults the
		// flag before DlssStartup / pointing volk at the interposer — after
		// that the decision is locked in) and force the mode off so no feature
		// evaluation happens either. This is the RenderDoc-safe configuration.
		// Low latency (NVIDIA Reflex / PCL) needs no separate switch here:
		// VK_NV_low_latency2 is enabled only while Streamline is loaded
		// (VulkanCore gates it on DlssAvailable()) and the PCL marker entry
		// points are silent no-ops without the interposer — no interposer,
		// no low-latency queue state, no markers.
		if (cfg.noDlss || cfg.capture) {
			smvk::DlssSetStartupEnabled(false);
			renderer.SetDlssMode(smvk::DlssMode::Off);
			std::cerr << "[DLSS] disabled (" << (cfg.capture ? "--capture implies it" : "--no-dlss")
				<< "): Streamline interposer not loaded\n";
		}
		// --capture: the one flag for the full RenderDoc launch configuration.
		// What breaks captures, and where each piece goes down:
		//   - DLSS / Streamline: the interposer sits in the Vulkan call chain
		//     and NGX evaluates from cubins RenderDoc cannot replay — crashes
		//     on capture (the gate above, implied from --no-dlss).
		//   - Low latency (Reflex / PCL): rides the same gate (see above) —
		//     not a separately switchable path.
		//   - Optick: forced off below, where the profiler gates are set
		//     before Run() (the GPU profiler's fence-based flip interleaves
		//     its own command-buffer submits with the frame — captures break
		//     with them in the mix).
		// Present mode is left alone (RenderDoc hooks any present mode); the
		// TOURMALINE_CAPTURE env pin (PNG screenshot export) is unrelated.
		if (cfg.capture)
			std::cerr << "[Capture] RenderDoc config: DLSS/Streamline off,"
			" low latency (Reflex/PCL) off with it, Optick forced off\n";
		if (cfg.disableControl) {
			element->SetDisableControl(true);
			std::cerr << "[Scene] camera control disabled (--disable_control)\n";
		}
		if (!cfg.presentMode.empty()) {
			if (cfg.presentMode == "fifo")
				app.SetPresentMode(smvk::PresentMode::Fifo);
			else if (cfg.presentMode == "mailbox")
				app.SetPresentMode(smvk::PresentMode::Mailbox);
			else if (cfg.presentMode == "immediate")
				app.SetPresentMode(smvk::PresentMode::Immediate);
			else {
				std::cerr << "Unknown present mode: '" << cfg.presentMode << "' (fifo|mailbox|immediate)\n";
				return EXIT_FAILURE;
			}
		}
		int benchFrames = cfg.benchFrames;
		if (benchFrames > 0) {
			app.SetFrameBudget(benchFrames);
			// Bench forces vsync off (uncapped immediate present): FIFO paces
			// every frame to the display interval, so the measured "avg frame"
			// reports the vsync period (16.7 ms @60) instead of the frame's
			// real cost — the [perf] numbers below are honest only uncapped.
			// An explicit --present is the more deliberate choice and wins
			// (same override philosophy as the RunConfig env pins).
			if (!cfg.presentSet) {
				app.SetPresentMode(smvk::PresentMode::Immediate);
				std::cout << "bench: vsync off (uncapped immediate present; "
					"--present <mode> overrides)\n";
			}
			// Bench captures must be pixel-comparable across runs: drop eye
			// adaptation and pin the fixed manual exposure (X re-enables live).
			// PINNED, not just set: the per-scene PushPostfxEnv seed inside
			// onAttach re-seeds auto-exposure from the authored
			// env (default ON), silently re-enabling adaptation mid-bench;
			// the pin survives the seed (see m_autoExposurePin).
			renderer.SetAutoExposurePinned(false);
			renderer.SetAutoExposureEnabled(false);
			std::cout << "bench: auto-exposure off (fixed manual exposure)\n";
		}
		if (cfg.benchMoveSpeed > 0.0f) {
			// Constant-velocity flight needs --bench to be meaningful (the
			// movement gates on the measured window); pair them up here so a
			// bare --bench_move still does the right thing.
			if (benchFrames <= 0) {
				benchFrames = 300;
				app.SetFrameBudget(benchFrames);
				if (!cfg.presentSet) {
					// Same vsync-off force as --bench above: uncapped timings.
					app.SetPresentMode(smvk::PresentMode::Immediate);
					std::cout << "bench: vsync off (uncapped immediate present; "
						"--present <mode> overrides)\n";
				}
				renderer.SetAutoExposurePinned(false);
				renderer.SetAutoExposureEnabled(false);
				std::cout << "bench: auto-exposure off (fixed manual exposure)\n";
			}
			element->SetBenchMoveSpeed(cfg.benchMoveSpeed);
			std::cout << "bench: constant-velocity flight " << cfg.benchMoveSpeed
				<< " m/s forward from the spawn pose (past warmup)\n";
		}
		// Host extension element (the game-host hook, see Host.h): stacked
		// AFTER the renderer element — attach order guarantees the world is
		// loaded (the renderer's onAttach ran the world source) before the
		// extension looks for it. Bench runs never stack it (the extension
		// animates every frame — capture determinism).
		const bool extraOn = static_cast<bool>(cfg.extraElement)
			&& cfg.benchFrames <= 0 && cfg.benchMoveSpeed <= 0.0f;
		if (extraOn && cfg.extraOwnsCamera)
			element->SetDisableControl(true); // the extension owns the camera
		// Attach the element to the host's stack and DROP the host-side
		// reference: the App owns the element now (Shutdown's detach pass
		// destroys it INSIDE the shutdown window, Holders releasing while
		// the context is alive — an outliving host reference would defer
		// element destruction to after ~App and leak the GPU handles).
		app.AddElement(element);
		element.reset();
		if (extraOn) {
			if (auto extra = cfg.extraElement(app, renderer, worldHandle)) {
				// Reverse detach order: the extension drops off the stack
				// (and out of the world) before the renderer tears down.
				app.AddElement(extra);
				std::cerr << "[Scene] host element '" << extra->ElementName()
					<< "' stacked over the renderer\n";
			}
		}
		// Optick gates must be set before Run(): the engine initializes the GPU
		// profiler during VulkanCore init, and the bench session starts after the
		// warmup frames. --use_optick_gpu implies --use_optick. --capture
		// force-disables both (the per-flip GPU submits break RenderDoc
		// captures) and wins over --use_optick[_gpu] if both are given.
		if (cfg.capture) {
			smvk::profiler::SetEnabled(false);
			smvk::profiler::SetGpuEnabled(false);
			std::cerr << "[Capture] Optick off (CPU + GPU): per-flip submits"
				" would break the capture\n";
		}
		else {
			smvk::profiler::SetEnabled(cfg.useOptick);
			if (cfg.useOptickGpu)
				smvk::profiler::SetGpuEnabled(true);
		}
		try {
			app.Run();
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

} // namespace app
