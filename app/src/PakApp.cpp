// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Pak-viewer host run: renderer setup + world source + main loop. This is
// the APP's host layer (moved out of the engine — the engine renders
// worlds, the app owns pak loading, check-scene extras and demo content).
// The pak -> World half lives in PakWorldLoader.cpp; this file is the
// viewer HOST: renderer wiring (passes/bench/DLSS/control), the element
// lifecycle, and the process exit-code contract. Identifiers: smvk:: =
// engine runtime, engine:: = the renderer layer.

#include "Host.h"
#include "PakPreload.h"
#include "PakWorldLoader.h"

#include <Engine/Asset/appmanifest.h>
#include <Engine/Memory/memory.h>
#include <Engine/Profile/profile.h>

#include <Engine/Renderer/WorldRenderer.h>
#include <Engine/Renderer/WorldRendererElement.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app {
	namespace {

		// Pak stem -> scene id (GI bake stem + check-scene gating). The
		// loader has its own copy for the anim fallback; the renderer's id
		// resolves here.
		std::string StemOf(const std::string& p) {
			const size_t slash = p.find_last_of("/\\");
			const std::string file = (slash == std::string::npos)
				? p : p.substr(slash + 1);
			const size_t dot = file.find_last_of('.');
			return (dot == std::string::npos || dot == 0)
				? file : file.substr(0, dot);
		}

		// Bisection: layer extra passes onto the base config
		// (csv: deferred,shadows,ssao/gtao/ao,smaa,motion,dof,ssr,bloom,exposure,msaa,skydome,ui,cull,particles).
		// PURE renderer-CONFIG parsing: it touches only `passCfg`. The
		// memory_aliasing token is NOT a pass bool — it maps to the
		// GLOBAL target-heap aliasing gate (smvk::SetTargetHeapAliasing),
		// and a configuration helper must not mutate global engine state
		// as a side effect: the token is REPORTED through
		// `outMemoryAliasing` and the CALLER applies it at its chosen
		// point (RunPakApp applies it at attach time, BEFORE any target
		// claims, so the CLI token wins over the env pin — see the
		// SetTargetHeapAliasing contract).
		void ApplyPassCsv(engine::WorldRendererConfig& passCfg, const char* csv,
			bool enable, std::optional<bool>* outMemoryAliasing) {
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
				// smvk::TargetHeapAliasingEnabled): not a pass bool - the
				// token REPORTS the requested global-gate state through the
				// out param; the caller owns when/whether to apply it.
				// --without memory_aliasing (default state) keeps every
				// packed range disjoint; --with memory_aliasing honors the
				// declared windows. Composes with the env pin
				// (TOURMALINE_ALIASING): the CLI token applies at attach
				// time, BEFORE any target claims - so it wins.
				else if (tok == "memory_aliasing" || tok == "aliasing") {
					if (outMemoryAliasing)
						*outMemoryAliasing = enable;
				}
				else if (!tok.empty()) std::cerr << "Unknown pass token: '" << tok << "'\n";
				if (flag) *flag = enable;
				pos = end + 1;
			}
		}

	} // namespace

	int RunPakApp(PakAppConfig cfg) {
		// The process exit code (App::Run()'s bool PROPAGATES: a renderer
		// failure used to exit 0, so launch scripts read success on a
		// dead viewer).
		int exitCode = EXIT_SUCCESS;
		// Reserve linear-allocator virtual address space FIRST, before worker
		// threads, Vulkan, or asset loading (see Engine/Memory/memory.h). An
		// unsatisfiable reservation fails fast here with the OOM diagnostic.
		smvk::mem::InitMemory();

		// Early gate: an unknown --present value fails fast, BEFORE any
		// window/context/element work (nothing to shut down but the memory
		// footprint report, same as the pre-wiring failures below).
		if (!cfg.cli.presentMode.empty()
			&& cfg.cli.presentMode != "fifo"
			&& cfg.cli.presentMode != "mailbox"
			&& cfg.cli.presentMode != "immediate") {
			std::cerr << "Unknown present mode: '" << cfg.cli.presentMode
				<< "' (fifo|mailbox|immediate)\n";
			return EXIT_FAILURE;
		}

		// Entrypoint: --pak (single-pak legacy) or --project (the
		// appmanifest launch). The manifest parses + validates HERE —
		// before any Vulkan work — and its pak list resolves
		// MANIFEST-RELATIVE (a moved build directory keeps working);
		// every listed pak preloads in parallel, mounts in listed
		// order, and the entry scene loads by identity at attach.
		std::string sceneId;
		std::shared_ptr<std::future<PakPreload>> preloadFuture;
		std::shared_ptr<std::vector<std::future<PakPreload>>>
			projectFutures;
		size_t entryPakIndex = 0;
		std::string entrySceneName;
		if (!cfg.cli.project.empty()) {
			smvk::AppManifest manifest;
			std::string mErr;
			if (!smvk::LoadAppManifest(cfg.cli.project, manifest, mErr)) {
				std::cerr << "[App] appmanifest rejected: " << mErr
					<< "\n";
				return EXIT_FAILURE;
			}
			namespace fs = std::filesystem;
			const fs::path base =
				fs::path(cfg.cli.project).lexically_normal()
					.parent_path();
			std::vector<std::string> pakPaths;
			bool entryFound = false;
			for (size_t i = 0; i < manifest.paks.size(); ++i) {
				fs::path p(manifest.paks[i]);
				if (p.is_relative())
					p = base / p;
				pakPaths.push_back(p.lexically_normal().generic_string());
				if (manifest.paks[i] == manifest.entryPak) {
					entryPakIndex = i;
					entryFound = true;
				}
			}
			if (!entryFound) { // validation guarantees this; belt+braces
				std::cerr << "[App] appmanifest entry pak '"
					<< manifest.entryPak << "' not listed\n";
				return EXIT_FAILURE;
			}
			entrySceneName = manifest.entryScene;
			sceneId = StemOf(pakPaths[entryPakIndex]);
			auto futures = std::make_shared<
				std::vector<std::future<PakPreload>>>();
			for (const std::string& pak : pakPaths)
				futures->push_back(KickPakPreload(pak));
			projectFutures = std::move(futures);
			std::cerr << "[App] project '" << cfg.cli.project << "': "
				<< manifest.paks.size() << " pak(s), entry '"
				<< manifest.entryPak << "' scene '" << entrySceneName
				<< "'\n";
			if (cfg.cli.noVfs)
				std::cerr << "[Scene] --no-vfs: only the project's "
					<< manifest.paks.size() << " mounted pak(s) serve"
					<< " scene content\n";
		}
		else {
			// A pak file + a scene index (0 = default). The scene id
			// (GI bake stem + check-scene gating) is the pak stem.
			sceneId = cfg.cli.sceneId.empty()
				? StemOf(cfg.cli.pakPath) : cfg.cli.sceneId;
			preloadFuture = std::make_shared<std::future<PakPreload>>(
				KickPakPreload(cfg.cli.pakPath));
			std::cerr << "Scene: pak '" << cfg.cli.pakPath << "' (id '"
				<< sceneId << "', index " << cfg.cli.sceneIndex << ")\n";
		}
		if (sceneId.empty())
			sceneId = "pak";

		// App host + WorldRenderer ELEMENT (nvapp-style: the host owns the
		// window/context/swapchain + the frame sync — the default DrawFrame
		// pump drives the element between its acquire and present; the
		// renderer never acquires/presents on its own).
		smvk::App app;
		try {
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
			// (The --project mode printed its own message above.)
			if (cfg.cli.noVfs && cfg.cli.project.empty()) {
				renderer.SetNoVfs(true);
				std::cerr << "[Scene] --no-vfs: only pak '" << cfg.cli.pakPath
					<< "' serves scene content\n";
			}
			// The HOST owns scene loading (PakWorldLoader): the renderer calls
			// this once during onAttach, after the host's context exists
			// (nothing can upload before that), and renders the returned world
			// by handles. By then the preload has typically been running for
			// the whole of app boot. The renderer's texture streamer rides
			// along so v4 digest textures register for demand upgrades (null
			// when streaming is off → full resident, the pre-v4 behavior). The
			// preloaded image pins the borrowed tail payloads. The lambda
			// captures the RAW renderer pointer (it runs once INSIDE the
			// element's onAttach, where the element is alive by definition) — a
			// shared_ptr capture would self-cycle.
			const int sceneIndex = cfg.cli.sceneIndex;
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
				[preloadFuture, projectFutures, entryPakIndex,
					entrySceneName, sceneIndex, rendererRaw, worldHandle]
				(
					smvk::ResourceManager& ctx,
					smvk::OverlayVfs& contentVfs
					) -> smvk::mem::LinearPtr<smvk::World> {
				// --project: every listed pak's preload joins here, the
				// backends mount in manifest order and the entry scene
				// loads BY IDENTITY (LoadProjectWorld). --pak: the
				// legacy single-pak, index-based path.
				smvk::mem::LinearPtr<smvk::World> world;
				if (projectFutures) {
					std::vector<PakPreload> preloads;
					preloads.reserve(projectFutures->size());
					for (std::future<PakPreload>& f : *projectFutures)
						preloads.push_back(f.get());
					world = LoadProjectWorld(std::move(preloads),
						entryPakIndex, entrySceneName, ctx, contentVfs,
						rendererRaw->GetTextureStreamer());
				}
				else {
					world = LoadPakWorld(preloadFuture->get(),
						sceneIndex,
						ctx,
						contentVfs,
						rendererRaw->GetTextureStreamer()
						);
				}
				*worldHandle = world.get();
				return world;
			});
			engine::WorldRendererConfig passCfg = engine::WorldRendererConfig::All();
			if (cfg.cli.filterOnly)
				passCfg = engine::WorldRendererConfig::FilterOnly();
			else if (cfg.cli.minimal)
				passCfg = engine::WorldRendererConfig::Minimal();
			std::optional<bool> memoryAliasing; // the parsed token, applied below
			ApplyPassCsv(passCfg, cfg.cli.withCsv.c_str(), true, &memoryAliasing);
			ApplyPassCsv(passCfg, cfg.cli.withoutCsv.c_str(), false, &memoryAliasing);
			renderer.Configure(passCfg, cfg.run);
			// The global memory-aliasing gate the token reported: applied
			// HERE, at wiring time — before app.Run() attaches the element
			// and any target claims happen, so the CLI token wins over the
			// TOURMALINE_ALIASING env pin (see smvk::SetTargetHeapAliasing).
			if (memoryAliasing.has_value())
				smvk::SetTargetHeapAliasing(*memoryAliasing);
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
			if (cfg.cli.noDlss || cfg.cli.capture) {
				smvk::DlssSetStartupEnabled(false);
				renderer.SetDlssMode(smvk::DlssMode::Off);
				std::cerr << "[DLSS] disabled (" << (cfg.cli.capture ? "--capture implies it" : "--no-dlss")
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
			if (cfg.cli.capture)
				std::cerr << "[Capture] RenderDoc config: DLSS/Streamline off,"
					" low latency (Reflex/PCL) off with it, Optick forced off\n";
			if (cfg.cli.disableControl) {
				element->SetDisableControl(true);
				std::cerr << "[Scene] camera control disabled (--disable_control)\n";
			}
			if (!cfg.cli.presentMode.empty()) {
				if (cfg.cli.presentMode == "fifo")
					app.SetPresentMode(smvk::PresentMode::Fifo);
				else if (cfg.cli.presentMode == "mailbox")
					app.SetPresentMode(smvk::PresentMode::Mailbox);
				else
					app.SetPresentMode(smvk::PresentMode::Immediate);
			}
			int benchFrames = cfg.cli.benchFrames;
			if (benchFrames > 0) {
				app.SetFrameBudget(benchFrames);
				// Bench forces vsync off (uncapped immediate present): FIFO paces
				// every frame to the display interval, so the measured "avg frame"
				// reports the vsync period (16.7 ms @60) instead of the frame's
				// real cost — the [perf] numbers below are honest only uncapped.
				// An explicit --present is the more deliberate choice and wins
				// (same override philosophy as the RunConfig env pins).
				if (!cfg.cli.presentSet) {
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
			if (cfg.cli.benchMoveSpeed > 0.0f) {
				// Constant-velocity flight needs --bench to be meaningful (the
				// movement gates on the measured window); pair them up here so a
				// bare --bench_move still does the right thing.
				if (benchFrames <= 0) {
					benchFrames = 300;
					app.SetFrameBudget(benchFrames);
					if (!cfg.cli.presentSet) {
						// Same vsync-off force as --bench above: uncapped timings.
						app.SetPresentMode(smvk::PresentMode::Immediate);
						std::cout << "bench: vsync off (uncapped immediate present; "
							"--present <mode> overrides)\n";
					}
					renderer.SetAutoExposurePinned(false);
					renderer.SetAutoExposureEnabled(false);
					std::cout << "bench: auto-exposure off (fixed manual exposure)\n";
				}
				element->SetBenchMoveSpeed(cfg.cli.benchMoveSpeed);
				std::cout << "bench: constant-velocity flight " << cfg.cli.benchMoveSpeed
					<< " m/s forward from the spawn pose (past warmup)\n";
			}
			// Host extension element (the game-host hook, see Host.h): stacked
			// AFTER the renderer element — attach order guarantees the world is
			// loaded (the renderer's onAttach ran the world source) before the
			// extension looks for it. Bench runs never stack it (the extension
			// animates every frame — capture determinism).
			const bool extraOn = static_cast<bool>(cfg.extraElement)
				&& cfg.cli.benchFrames <= 0 && cfg.cli.benchMoveSpeed <= 0.0f;
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
			if (cfg.cli.capture) {
				smvk::profiler::SetEnabled(false);
				smvk::profiler::SetGpuEnabled(false);
				std::cerr << "[Capture] Optick off (CPU + GPU): per-flip submits"
					" would break the capture\n";
			}
			else {
				smvk::profiler::SetEnabled(cfg.cli.useOptick);
				if (cfg.cli.useOptickGpu)
					smvk::profiler::SetGpuEnabled(true);
			}
			// EXCEPTION-SAFE LIFECYCLE: everything from the element construction
			// through the pump is inside the try. App::Run() shuts down on its
			// own SUCCESS/FAILURE paths; an EXCEPTION bypasses its internal
			// Shutdown, so the catch runs it explicitly — the element stack
			// must detach (Holders release) while the graphics members are
			// still alive, never during ~App unwinding against a dead context
			// (the ~App assert catches the forgotten call in debug). No
			// interactive prompt on the failure path: a headless run (bench
			// hosts, capture scripts) must EXIT, not hang on stdin.
			if (!app.Run())
				exitCode = EXIT_FAILURE;
		}
		catch (const std::exception& e) {
			std::cerr << "\nERROR: " << e.what() << std::endl;
			app.Shutdown();
			exitCode = EXIT_FAILURE;
		}
		catch (...) {
			std::cerr << "\nERROR: unknown exception" << std::endl;
			app.Shutdown();
			exitCode = EXIT_FAILURE;
		}
		smvk::mem::ShutdownMemory(); // footprint report (used/committed/reserved)
		return exitCode;
	}

} // namespace app
