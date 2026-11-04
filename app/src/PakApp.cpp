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
#include "ProjectRuntime.h"
#include "ShapeSceneLoader.h"
#include "DeployCheck.h"

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/cluster_serializer.h> // ListPakScenes (the discovery lane's entry pick)
#include <Engine/Asset/pak_pager.h>
#include <Engine/Memory/memory.h>
#include <Engine/Profile/profile.h>

#include <Engine/Renderer/WorldRenderer.h>
#include <Engine/Renderer/WorldRendererElement.h>
#include <Engine/Renderer/geometry_streamer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
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
                else if (tok == "ssgi") flag = &passCfg.ssgiDiffuse;
                else if (tok == "volumetric") flag = &passCfg.volumetric;
                else if (tok == "contact") flag = &passCfg.contactShadows;
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
		tour::mem::InitMemory();

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
		// Every pak this launch opens (both lanes) — the Stage-6 LOD
		// page source's pager set below.
		std::vector<std::string> launchPaks;
		// THE SHARED CONTENT SESSION (the --project lane): one
		// ProjectRuntime over the content registry — the manifest
		// adopted, the loose discovery tree registered as an override,
		// every preloaded backend adopted into the registry as the
		// preloads resolve (LoadProjectWorld does the adoption). The
		// world load's cross-pak resolver AND the game element's byte
		// reads resolve through the SAME provider selection (the
		// convergence contract — no per-layer package universes).
		std::shared_ptr<app::ProjectRuntime> session;
		if (!cfg.cli.project.empty()) {
			smvk::AppManifest manifest;
			std::string mErr;
			if (!smvk::LoadAppManifest(cfg.cli.project, manifest, mErr)) {
				std::cerr << "[App] appmanifest rejected: " << mErr
					<< "\n";
				return EXIT_FAILURE;
			}
			// THE SESSION: the manifest adopts into the registry (the
			// base providers + the record table) before any graphics
			// work — a manifest that does not adopt fails the launch
			// loudly right here.
			std::string sErr;
			if (auto rt = app::ProjectRuntime::Load(cfg.cli.project,
					sErr)) {
				session = std::make_shared<app::ProjectRuntime>(
					std::move(*rt));
				// THE COMPILED-CATALOG GATE (fatal bootstrap
				// validation): the game proves ITSELF against the
				// deployed content BEFORE any preload, any Vulkan
				// work — a mismatch is a REFUSED launch, never the
				// old log-and-run-anyway.
				if (cfg.compiledCatalogGate) {
					std::string gErr;
					if (!cfg.compiledCatalogGate(*session, gErr)) {
						std::cerr << "[App] the compiled catalog was"
							" REJECTED: " << gErr << "\n";
						return EXIT_FAILURE;
					}
					std::cerr << "[App] compiled catalog attached +"
						" validated against the deployment\n";
				}
				// The loose discovery tree (the renderer's
				// MountContentVfs equivalent) as a registry OVERRIDE:
				// dev-override files win over the paks for BOTH the
				// id form and the path form (the old split — the
				// overlay saw loose files, the [ids] join did not —
				// is the divergence this session exists to close).
				// --no-vfs mutes the lane: the project's paks are
				// then the ONLY providers.
				if (!cfg.cli.noVfs) {
					std::string oErr;
					(void)session->AddLooseOverride("content",
						"content", oErr);
				}
			}
			else {
				std::cerr << "[App] the content session failed: "
					<< sErr << "\n";
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
			launchPaks = pakPaths;
			// THE DEPLOYMENT VALIDATION GATE (pre-graphics, see
			// DeployCheck.h): every listed pak proves itself (exists +
			// a valid image + the content-addressed NAME matching the
			// actual bytes) and an engine shader source exists — a
			// broken deployment is a loud, actionable failure, never
			// a half-booted window. (The preload below re-opens the
			// images anyway; this gate runs FIRST and checks the
			// deployment CONTRACT the preload cannot see.)
			for (const std::string& pak : pakPaths) {
				std::string dErr;
				if (!app::ValidatePakFile(pak, dErr)) {
					std::cerr << "[App] deployment rejected: " << dErr
						<< "\n";
					return EXIT_FAILURE;
				}
			}
			{
				std::string dErr;
				if (!app::ValidateEngineShaderSource(dErr)) {
					std::cerr << "[App] deployment rejected: " << dErr
						<< "\n";
					return EXIT_FAILURE;
				}
			}
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
					<< manifest.paks.size() << " pak provider(s) serve"
					<< " content (the session carries no loose"
					<< " override — the id form and the path form"
					<< " agree on the pak-only universe)\n";
		}
		else {
			// THE PAK LANES. Legacy single-pak (no --scene, ONE --pak):
			// a pak file + a scene index (0 = default); the scene id
			// (GI bake stem + check-scene gating) is the pak stem.
			// The MULTI-PAK DISCOVERY LANE (several --pak, or --scene
			// naming the entry by its cooked identity): every pak
			// joins ONE content session (OpenDiscovered — a pak
			// carrying its "#ids" identity table contributes its ids;
			// a legacy pak serves paths only), DeployCheck polices each
			// archive like a --project launch, and the load runs the
			// SAME LoadProjectWorld path (the backends adopt into the
			// session as the preloads resolve; the entry scene is
			// --scene's name or the first scene-bearing pak's).
			const bool multiPak = cfg.cli.pakPaths.size() > 1
				|| !cfg.cli.sceneName.empty();
			if (multiPak) {
				for (const std::string& pak : cfg.cli.pakPaths)
					launchPaks.push_back(ResolvePakOpenPath(pak));
				sceneId = cfg.cli.sceneId.empty()
					? StemOf(cfg.cli.pakPaths.front())
					: cfg.cli.sceneId;
				// The content session over the discovered paks.
				std::string dErr;
				if (auto rt = app::ProjectRuntime::OpenDiscovered(
						launchPaks, dErr,
						/*requireIdentityBlob=*/false)) {
					session = std::make_shared<app::ProjectRuntime>(
						std::move(*rt));
					if (!cfg.cli.noVfs) {
						std::string oErr;
						(void)session->AddLooseOverride("content",
							"content", oErr);
					}
				}
				else {
					std::cerr << "[App] the content session failed: "
						<< dErr << "\n";
					return EXIT_FAILURE;
				}
				// The deployment gate (the --project contract): every
				// named pak proves itself BEFORE any graphics work.
				for (const std::string& pak : launchPaks) {
					std::string pErr;
					if (!app::ValidatePakFile(pak, pErr)) {
						std::cerr << "[App] deployment rejected: "
							<< pErr << "\n";
						return EXIT_FAILURE;
					}
				}
				{
					std::string sErr;
					if (!app::ValidateEngineShaderSource(sErr)) {
						std::cerr << "[App] deployment rejected: "
							<< sErr << "\n";
						return EXIT_FAILURE;
					}
				}
				auto futures = std::make_shared<
					std::vector<std::future<PakPreload>>>();
				for (const std::string& pak : launchPaks)
					futures->push_back(KickPakPreload(pak));
				projectFutures = std::move(futures);
				std::cerr << "[App] multi-pak discovery: "
					<< launchPaks.size() << " pak(s), entry scene "
					<< (cfg.cli.sceneName.empty()
						? "<first scene-bearing pak>"
						: ("'" + cfg.cli.sceneName + "'"))
					<< " (session " << session->Registry().RecordCount()
					<< " identity row(s))\n";
			}
			else {
				sceneId = cfg.cli.sceneId.empty()
					? StemOf(cfg.cli.pakPath) : cfg.cli.sceneId;
				// The RESOLVED pak path (the CLI arg may be relative —
				// the preload ladder resolves it; the Stage-6 pager needs
				// the same absolute path).
				launchPaks.push_back(ResolvePakOpenPath(cfg.cli.pakPath));
				// THE SINGLE-PAK LANE'S SESSION (lenient discovery over
				// the one archive — a legacy pak joins as a path-serving
				// provider, no identity rows) + the loose override:
				// EVERY content read on this path (the anim-clip
				// fallback inside PostLoadTail) resolves through the
				// SAME provider selection as every other lane — the
				// host overlay is no longer a content source anywhere.
				std::string dErr;
				if (auto rt = app::ProjectRuntime::OpenDiscovered(
						launchPaks, dErr,
						/*requireIdentityBlob=*/false)) {
					session = std::make_shared<app::ProjectRuntime>(
						std::move(*rt));
					if (!cfg.cli.noVfs) {
						std::string oErr;
						(void)session->AddLooseOverride("content",
							"content", oErr);
					}
				}
				else {
					std::cerr << "[App] the content session failed: "
						<< dErr << "\n";
					return EXIT_FAILURE;
				}
				preloadFuture = std::make_shared<std::future<PakPreload>>(
					KickPakPreload(launchPaks.back()));
				std::cerr << "Scene: pak '" << cfg.cli.pakPath << "' (id '"
					<< sceneId << "', index " << cfg.cli.sceneIndex << ")\n";
			}
		}
		if (sceneId.empty())
			sceneId = "pak";

		// STAGE 6 (geometry_streamer.h): when streaming is armed,
		// install the LOD PAGE SOURCE — the deferred-sibling promotes
		// read their ENTRY BYTES through the async pager instead of
		// the pinned whole image (the page IS the entry payload: a
		// complete contiguous blob). A pak that fails its light
		// mount is SKIPPED (logged) — its clusters keep the classic
		// image-pin promote lane.
		if (tour::GeometryStreamingEnabled()) {
			auto pagers = std::make_shared<
				std::vector<std::shared_ptr<smvk::PakPager>>>();
			for (const std::string& pak : launchPaks) {
				auto pager = std::make_shared<smvk::PakPager>(pak);
				if (pager->Valid())
					pagers->push_back(std::move(pager));
				else
					std::cerr << "[GeometryStreaming] the LOD page"
						" source skips '" << pak << "': "
						<< pager->OpenError() << "\n";
			}
			tour::SetLodPageSource(
				[pagers](const char* uri)
					-> std::optional<tour::LodPagePin> {
				for (const std::shared_ptr<smvk::PakPager>& p :
						*pagers) {
					const smvk::PakTocEntry* e =
						p->Reader().Find(smvk::PakAssetKind::Mesh,
							uri);
					if (!e)
						continue;
					std::string rerr;
					smvk::PakPager::PinnedPage page =
						p->Read(*e, rerr);
					if (!page)
						return std::nullopt; // the promote caller
						                     // reports the miss
					tour::LodPagePin pin;
					pin.data = page.data();
					pin.size = page.size();
					// The PIN rides the release closure through a
					// shared holder (std::function requires
					// copy-constructible targets; the move-only
					// PinnedPage cannot ride it directly): the LAST
					// closure copy's destruction — after the dispatch
					// scope drops the LodPagePin — unpins the pager
					// page; the upload itself is synchronous staging,
					// so the bytes are consumed before the release.
					auto held =
						std::make_shared<smvk::PakPager::PinnedPage>(
							std::move(page));
					pin.release = [held]() { (void)held; };
					return pin;
				}
				return std::nullopt;
			});
			std::cerr << "[GeometryStreaming] the LOD page source"
				" carries " << pagers->size() << " pak(s)\n";
		}

		// App host + WorldRenderer ELEMENT (nvapp-style: the host owns the
		// window/context/swapchain + the frame sync — the default DrawFrame
		// pump drives the element between its acquire and present; the
		// renderer never acquires/presents on its own).
		tour::App app;
		try {
			// Seed BEFORE service/game elements attach, and record the effective
			// seed even on unpinned launches so it can be replayed later.
			const uint64_t seed = cfg.run.rngSeed ? *cfg.run.rngSeed
				: tour::RandomService::GenerateSeed();
			app.Random().Reseed(seed);
			std::cerr << "[Rng] algorithm=pcg32-xsh-rr-v1 seed=" << seed << "\n";
			// The ELEMENT owns the control layer (input, keybinds, camera drive,
			// UI mode, lifecycle) and wraps the pure frame renderer (see
			// WorldRendererElement.h). Host config drives the renderer through
			// the element's Renderer() directly; control (disable_control /
			// bench_move) goes through the element.
			auto element = std::make_shared<engine::WorldRendererElement>(app);
			engine::WorldRenderer& renderer = element->Renderer();
			renderer.SetSceneId(sceneId);
            // Snapshot the COOKED launch identity, not the editor's live
            // world. The benchmark target consumes these same App flags.
            std::vector<std::string> benchmarkSourceArgs;
            if (!cfg.cli.project.empty()) {
                benchmarkSourceArgs = {"--project", cfg.cli.project};
            } else {
                for (const std::string& pak : cfg.cli.pakPaths) {
                    benchmarkSourceArgs.push_back("--pak");
                    benchmarkSourceArgs.push_back(pak);
                }
                if (!cfg.cli.sceneName.empty()) {
                    benchmarkSourceArgs.push_back("--scene");
                    benchmarkSourceArgs.push_back(cfg.cli.sceneName);
                } else if (cfg.cli.sceneIndex != 0) {
                    benchmarkSourceArgs.push_back("--scene-index");
                    benchmarkSourceArgs.push_back(std::to_string(cfg.cli.sceneIndex));
                }
            }
            renderer.SetBenchmarkSourceArgs(std::move(benchmarkSourceArgs));
            std::vector<std::string> benchmarkHostArgs;
            if (cfg.cli.minimal) benchmarkHostArgs.emplace_back("--minimal");
            if (cfg.cli.filterOnly) benchmarkHostArgs.emplace_back("--filter-only");
            if (cfg.cli.noVfs) benchmarkHostArgs.emplace_back("--no-vfs");
            if (cfg.cli.noDlss) benchmarkHostArgs.emplace_back("--no-dlss");
            renderer.SetBenchmarkAppArgs(std::move(benchmarkHostArgs));
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
			// The discovery lane's entry pick: --scene's name (empty =
			// the first scene-bearing pak's first scene), resolved
			// INSIDE the world-source call where the preloaded
			// readers exist. The --project lane's entry is
			// entryByIdentity (the manifest recorded it — never a
			// runtime pick).
			const std::string discoveryScene = cfg.cli.sceneName;
			const bool entryByIdentity = !cfg.cli.project.empty();
			// World handoff for the invaders element (app/src/InvadersGame):
			// the lambda stashes the raw world pointer as it returns the
			// LinearPtr to the renderer. The renderer owns the lifetime; the
			// raw pointer stays valid for the game element's whole run (the
			// element stack detaches in reverse order, so the game drops the
			// hook + stops touching the world before the renderer tears it
			// down).
			auto worldHandle = std::make_shared<tour::World*>();
			renderer.SetWorldSource(
				[preloadFuture, projectFutures, entryPakIndex,
					entrySceneName, entryByIdentity, discoveryScene,
					sceneIndex, rendererRaw, worldHandle,
					session]
				(
					smvk::ResourceManager& ctx,
					smvk::OverlayVfs& contentVfs
					) -> tour::mem::LinearPtr<tour::World> {
				// --project: every listed pak's preload joins here, the
				// backends mount in manifest order (adopted into the
				// SESSION's registry — the shared images) and the entry
				// scene loads BY IDENTITY (LoadProjectWorld; the
				// cross-pak resolver reads through the session).
				// Multi-pak discovery: the same path, the entry picked
				// from the preloaded readers. --pak (single): the legacy
				// single-pak, index-based path.
				tour::mem::LinearPtr<tour::World> world;
				std::string packedShapePlacements;
				auto captureShapePlacements = [&](const PakPreload& preload) {
					if (!preload.backend) return true; // loader reports this failure
					const auto* e = preload.backend->Reader().Find(
						smvk::PakAssetKind::ShapePlacements,
						tour::asset::kShapePlacementsPakPath);
					if (!e) return true; // legacy scene without shape instances
					try {
						const auto payload = preload.backend->Reader().Payload(*e);
						packedShapePlacements.assign(
							reinterpret_cast<const char*>(payload.data()),
							payload.size());
						return true;
					} catch (const std::exception& ex) {
						std::cerr << "[Scene] shape placement sidecar corrupt: "
							<< ex.what() << "\n";
						return false;
					}
				};
				if (projectFutures) {
					std::vector<PakPreload> preloads;
					preloads.reserve(projectFutures->size());
					for (std::future<PakPreload>& f : *projectFutures)
						preloads.push_back(f.get());
					size_t entryIdx = entryPakIndex;
					std::string entryName = entrySceneName;
					if (!entryByIdentity) {
						// The discovery lane's pick: --scene's name
						// across the preloaded paks, else the FIRST
						// scene-bearing pak's first scene. A named scene
						// that no pak carries fails loudly — never a
						// fallback, never index 0 by accident.
						bool found = false;
						for (size_t pi = 0; pi < preloads.size()
								&& !found; ++pi) {
							if (!preloads[pi].backend)
								continue;
							std::string lerr;
							const std::vector<smvk::PakSceneInfo>
								scenes = smvk::ListPakScenes(
									preloads[pi].backend->Reader(),
									&lerr);
							if (scenes.empty())
								continue;
							if (!discoveryScene.empty()) {
								for (size_t si = 0; si < scenes.size();
										++si)
									if (scenes[si].name
											== discoveryScene) {
										entryIdx = pi;
										entryName = scenes[si].name;
										found = true;
										break;
									}
							}
							else {
								entryIdx = pi;
								entryName = scenes[0].name;
								found = true;
							}
						}
						if (!found) {
							std::cerr << "[App] the entry scene "
								<< (discoveryScene.empty()
									? std::string("(any)")
									: ("'" + discoveryScene + "' "))
								<< "is not in any of the "
								<< preloads.size() << " pak(s) — scenes"
									" load BY IDENTITY, a miss fails"
									" loudly (never a fallback)\n";
							return nullptr;
						}
					}
					if (entryIdx >= preloads.size()
							|| !captureShapePlacements(preloads[entryIdx]))
						return nullptr;
					world = LoadProjectWorld(std::move(preloads),
						entryIdx, entryName, ctx, contentVfs,
						rendererRaw->GetTextureStreamer(), session.get());
				}
				else {
					PakPreload entryPreload = preloadFuture->get();
					if (!captureShapePlacements(entryPreload))
						return nullptr;
					world = LoadPakWorld(std::move(entryPreload),
						sceneIndex,
						ctx,
						contentVfs,
						rendererRaw->GetTextureStreamer(),
						*session
						);
				}
				// Load the packaged entry scene's retained shapes. Additional
				// scenes use ProjectScenes' per-SceneHandle ownership lane.
				if (world && !packedShapePlacements.empty()) {
					std::string shapeError;
					if (!LoadPackedShapePlacements(packedShapePlacements,
							*session, rendererRaw->Shapes(), shapeError)) {
						std::cerr << "[Scene] entry shapes failed: "
							<< shapeError << "\n";
						rendererRaw->Shapes().Clear();
						return nullptr;
					}
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
			if (cfg.cli.maxFps > 0)
				app.SetMaxFps(static_cast<uint32_t>(cfg.cli.maxFps));
			if (!cfg.cli.inputRecordPath.empty() ||
				!cfg.cli.inputReplayPath.empty()) {
				std::string inputError;
				const bool ok = !cfg.cli.inputRecordPath.empty()
					? app.ConfigureInputRecording(cfg.cli.inputRecordPath, inputError)
					: app.ConfigureInputReplay(cfg.cli.inputReplayPath, inputError);
				if (!ok)
					throw std::runtime_error("input tape: " + inputError);
			}
			if (!cfg.cli.snapshotSequenceDir.empty()) {
				std::string snapshotError;
				if (!app.ConfigureSnapshotInputCapture(snapshotError))
					throw std::runtime_error("snapshot input: " + snapshotError);
				std::error_code ec;
				const std::filesystem::path directory(cfg.cli.snapshotSequenceDir);
				std::filesystem::create_directories(directory, ec);
				if (ec)
					throw std::runtime_error("cannot create snapshot directory: " + ec.message());
				const auto captureTime = std::chrono::system_clock::now();
				const auto captureUnixMs = std::chrono::duration_cast<
					std::chrono::milliseconds>(captureTime.time_since_epoch()).count();
				const auto identity = std::to_string(seed) + "-" +
					std::to_string(captureTime.time_since_epoch().count());
				const auto sequenceId = tour::snapshot::Fnv64(identity);
				tour::snapshot::Json provenance = {
					{"rng_seed", seed},
					{"rng_algorithm", "pcg32-xsh-rr-v1"},
					{"source_args", cfg.cli.project.empty()
						? tour::snapshot::Json(cfg.cli.pakPaths)
						: tour::snapshot::Json(cfg.cli.project)},
					{"source_kind", cfg.cli.project.empty() ? "pak" : "project"},
					{"scene", sceneId},
					{"code_identity", cfg.cli.snapshotCodeId.empty()
						? "unspecified" : cfg.cli.snapshotCodeId},
					{"game_code_included", false},
					{"catalog_fingerprint", session
						? std::to_string(session->Registry().RecordFingerprint()) : "unavailable"},
					{"catalog_manifest_version", session
						? session->Manifest().version : 0u},
					{"catalog_generation_timestamp_unix_ms", nullptr},
					{"capture_unix_time_ms", captureUnixMs},
					{"catalog_source_mtime", nullptr},
					{"state_scope", "engine-scene-graph-v1"}
				};
				if (!cfg.cli.project.empty()) {
					const auto time = std::filesystem::last_write_time(
						std::filesystem::path(cfg.cli.project), ec);
					if (!ec)
						provenance["catalog_source_mtime"] =
							std::to_string(time.time_since_epoch().count());
				}
				// Record available source-file timestamps, including all
				// discovered paks. File-clock ticks are for provenance,
				// not a portable content fingerprint.
				provenance["pak_sources"] = tour::snapshot::Json::array();
				for (const auto& pakPath : launchPaks) {
					std::error_code stampError;
					const auto stamp = std::filesystem::last_write_time(
						std::filesystem::path(pakPath), stampError);
					provenance["pak_sources"].push_back({
						{"path", pakPath},
						{"modified_file_clock_ticks", stampError
							? tour::snapshot::Json(nullptr) : tour::snapshot::Json(
								std::to_string(stamp.time_since_epoch().count()))}
					});
				}
				if (!renderer.ConfigureSnapshotSequence(directory,
					cfg.cli.snapshotKeyframeInterval, cfg.cli.snapshotCount,
					sequenceId, std::move(provenance), snapshotError))
					throw std::runtime_error("snapshot sequence: " + snapshotError);
				app.SetRunFinalizer([&renderer]() {
					return renderer.FinishSnapshotSequence();
				});
				// Snapshot count is exact, unlike benchmark's 10 warmup
				// + measured budget. Bench users may explicitly run longer.
				if (cfg.cli.benchFrames <= 0)
					app.SetTotalFrameLimit(cfg.cli.snapshotCount);
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
			// Host extension elements (the game-host hook, see Host.h):
			// stacked AFTER the renderer element, IN LIST ORDER -
			// attach order guarantees the world is loaded (the
			// renderer's onAttach ran the world source) before the
			// extensions look for it, and the reverse detach order
			// drops them before the renderer tears the world down.
			// Bench runs never stack them (the extensions animate
			// every frame - capture determinism), unless the explicit
			// TOURMALINE_EXTRA_ELEMENT_BENCH=1 pin opts in: the
			// headless A/B lane for extension-driven effects (the
			// ProjectLit family demo's capture, e.g.
			// TOURMALINE_CAPTURE=12 --bench 24 with the pin).
			const bool extraOn = !cfg.extraElements.empty()
				&& (cfg.cli.benchFrames <= 0 && cfg.cli.benchMoveSpeed <= 0.0f
					|| [] {
						static const bool pin = [] {
							const char* e =
								std::getenv("TOURMALINE_EXTRA_ELEMENT_BENCH");
							return e && std::strcmp(e, "1") == 0;
						}();
						return pin;
					}());
			if (extraOn && cfg.extraOwnsCamera)
				element->SetDisableControl(true); // an extension owns the camera
			// Attach the element to the host's stack and DROP the host-side
			// reference: the App owns the element now (Shutdown's detach pass
			// destroys it INSIDE the shutdown window, Holders releasing while
			// the context is alive - an outliving host reference would defer
			// element destruction to after ~App and leak the GPU handles).
			app.AddElement(element);
			element.reset();
			for (const auto& factory : cfg.extraElements) {
				if (auto extra = factory(app, renderer, worldHandle,
						session)) {
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
		tour::mem::ShutdownMemory(); // footprint report (used/committed/reserved)
		return exitCode;
	}

} // namespace app
