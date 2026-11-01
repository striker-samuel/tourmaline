// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The CONTENT-AS-CODE sample: a standalone project exe. The generated
// project.h (the content ids from the project's asset database —
// samples/04_project_game/project) is a pure constexpr catalog: this
// code programs against stable ids, never paths, never scene indices.
// The GameProject_paks target cooks the paks + the appmanifest next to
// the exe on demand (cmake --build --target GameProject_paks); the
// apphost resolves them exe-relative at run time.
//
//   cmake --build out/build/x64-Release --target GameProject       (exe + project.h)
//   cmake --build out/build/x64-Release --target GameProject_paks  (content)
//   out/build/x64-Release/GameProject.exe                          (runs the entry scene)

#include "Host.h"
#include "ProjectRuntime.h"
#include "AudioRuntime.h"

#include "project.h"

#include <Engine/App/Element.h>
#include <Engine/App/RunConfig.h>
#include <Engine/App/world.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h> // GetModuleFileNameA (the exe-relative manifest)
#else
#include <unistd.h> // readlink (/proc/self/exe)
#endif

// ---- THE PROJECT LIT FAMILY DEMO (docs/shader_library_design.md
// Phase B) -----------------------------------------------
// The two worked-example effects — enemy damage flash (worked example
// A) and runtime wetness (worked example B) — shipped as the sample's
// demonstration material: the scene's first opaque material opts into
// the ProjectLit family (the project-authored lit_hooks_project.hlsli
// compiled into project_lit.*.spv) and a per-frame pulse drives both
// userdata bit fields through the family's hooks. No shader recompile,
// no reupload, no batch split — a family switch is a pipeline pick,
// a userdata change is a CPU write.
//
// The element COMPOSES with the audio runtime (RunPakApp carries one
// extraElement slot): lifecycle forwards to the nested element, the
// demo work rides onUpdate beside it.
class ProjectLitDemo final : public engine::IAppElement {
public:
	ProjectLitDemo(std::shared_ptr<engine::IAppElement> audio,
		std::shared_ptr<smvk::World*> worldHandle)
		: m_audio(std::move(audio))
		, m_worldHandle(std::move(worldHandle)) {}

	const char* ElementName() const override { return "GameProjectDemo"; }

	bool onAttach() override {
		// The host stacks this element AFTER the renderer's attach (the
		// world source ran), so the scene's materials exist here.
		AdoptMaterial();
		return m_audio ? m_audio->onAttach() : true;
	}
	void onDetach() override { if (m_audio) m_audio->onDetach(); }
	void onResize(smvk::Extent2D e) override { if (m_audio) m_audio->onResize(e); }
	void onUpdate(float dt) override {
		if (m_audio)
			m_audio->onUpdate(dt);
		Pulse(dt);
	}
	void onUIMenu() override { if (m_audio) m_audio->onUIMenu(); }
	void onUIRender() override { if (m_audio) m_audio->onUIRender(); }
	void onPreRender(engine::ElementFrame& f) override {
		if (m_audio) m_audio->onPreRender(f);
	}
	void onRender(engine::ElementFrame& f) override {
		if (m_audio) m_audio->onRender(f);
	}

private:
	// Picks the scene's first opaque material and opts it into the
	// family. A missing family (engine-only deployment, no
	// project_lit.*.spv) is NOT an error: the renderer's consult
	// reports the StandardLit fallback loudly and the demo simply has
	// no effect to drive.
	void AdoptMaterial() {
		smvk::World* world = m_worldHandle ? *m_worldHandle : nullptr;
		if (!world)
			return;
		world->forEachRenderItem(
			smvk::IteratorFlagBit_Renderable
			| smvk::IteratorFlagBit_Opaque
			| smvk::IteratorFlagBit_StaticVertexFmt,
			[&](const smvk::World::RenderItem& item) {
			if (m_material != smvk::World::kInvalidIndex)
				return;
			if (item.material == smvk::World::kInvalidIndex)
				return;
			m_material = item.material;
		});
		if (m_material == smvk::World::kInvalidIndex)
			return;
		world->SetMaterialShaderFamily(m_material,
			smvk::MaterialShaderFamily::ProjectLit);
		std::printf("[GameProject] ProjectLit family: material %u opts in"
			" (damage+wetness pulse through the hooks)\n", m_material);
	}
	// The userdata bit layout the hooks read (lit_hooks_project.hlsli):
	// bits 0..7 damage, 8..15 wetness. Two slow out-of-phase pulses so
	// both effects are visible; a plain CPU write per frame.
	void Pulse(float dt) {
		smvk::World* world = m_worldHandle ? *m_worldHandle : nullptr;
		if (!world || m_material == smvk::World::kInvalidIndex)
			return;
		m_time += dt;
		const auto quant = [](float v) -> uint32_t {
			if (!(v > 0.0f))
				return 0u;
			if (v >= 1.0f)
				return 255u;
			return uint32_t(v * 255.0f);
		};
		const float damage = 0.5f + 0.5f * std::sin(m_time * 1.7f);
		const float wet = 0.5f + 0.5f * std::sin(m_time * 0.6f + 2.1f);
		world->SetMaterialUserData(m_material,
			quant(damage) | (quant(wet) << 8));
	}

	std::shared_ptr<engine::IAppElement> m_audio;
	std::shared_ptr<smvk::World*> m_worldHandle;
	uint32_t m_material = smvk::World::kInvalidIndex;
	float m_time = 0.0f;
};

int main(int argc, const char* argv[]) {
	// COMPILE-TIME content proofs: the catalog is real C++ — the entry
	// scene exists, the pak table is populated, the ids are constexpr.
	static_assert(project::Contains(project::EntryScene),
		"the catalog must carry the entry scene");
	static_assert(!project::Paks.empty(),
		"the catalog must carry at least one pak");
	static_assert(project::Contains(project::textures::stone),
		"the catalog must carry the shared texture");
	// THE QUERY API pins (docs/content_as_code.md): MatchAssets'
	// consteval regex over the generated symbol table. New assets
	// matching a pattern join the result AUTOMATICALLY at the next
	// project.h regeneration + rebuild.
	static_assert(project::MatchAssets<project::MeshKey, "^game.*$">()
		.size() == 1);
	static_assert(project::MatchAssets<project::MeshKey, "^game.*$">()[0]
		.id == project::meshes::game_2.id);
	static_assert(project::MatchAssets<project::TextureKey, "^stone$">()
		.size() == 1);
	static_assert(project::MatchAssets<project::SoundKey, "^bell.*$">()
		.size() == 1);
	// Kind filtering + empty results + the PATH field.
	static_assert(project::MatchAssets<project::SceneKey, "^game.*$">()
		.size() == 1);
	static_assert(project::MatchAssets<project::ShaderKey, "^.*$">()
		.size() == 0);
	static_assert(project::MatchAssets<project::GiKey, "^.*$">()
		.size() == 0);
	static_assert(project::MatchAssets<project::TextureKey,
		"^content/models/game/shared/.*\\.png$",
		::project_pipeline::query::MatchField::Path>().size() == 1);

	// The catalog-driven launch banner (every id the header carries).
	std::printf("[GameProject] content-as-code: project 0x%llX '%s', %zu"
		" catalog asset(s)\n",
		(unsigned long long)project::ProjectId,
		std::string(project::Name).c_str(),
		project::Assets.size());
	for (const auto& pak : project::Paks)
		std::printf("[GameProject] pak '%s' -> %s\n",
			std::string(pak.name).c_str(),
			std::string(pak.file).c_str());
	std::printf("[GameProject] entry scene id 0x%llx (identity launch,"
		" never an index)\n",
		(unsigned long long)project::EntryScene);

	// Central run config (the TOURMALINE_* pins) + the host assembly:
	// same host as the plain viewer, but the ENTRY POINT comes from the
	// PROJECT (the appmanifest beside the exe) unless the caller names
	// one explicitly (--pak/--project still work for debugging).
	const engine::RunConfig runCfg = engine::RunConfig::Parse();
	runCfg.Log();
	app::PakAppConfig cfg;
	cfg.run = runCfg;

	std::vector<const char*> args(argv, argv + argc);
	std::string manifestPath;
	bool hasEntry = false;
	for (int i = 1; i < argc; ++i)
		if (std::strncmp(argv[i], "--pak", 5) == 0
				|| std::strncmp(argv[i], "--project", 9) == 0)
			hasEntry = true;
	if (!hasEntry) {
		// The packaged launch: the appmanifest next to this exe (the
		// GameProject_paks target publishes it here; its paks resolve
		// manifest-relative so the exe+content pair moves as one).
		namespace fs = std::filesystem;
		char exePath[1024] = {};
#ifdef _WIN32
		GetModuleFileNameA(nullptr, exePath, DWORD(sizeof(exePath)));
#else
		const ssize_t n = readlink("/proc/self/exe", exePath,
			sizeof(exePath) - 1);
		if (n > 0)
			exePath[n] = '\0';
#endif
		manifestPath = (fs::path(exePath).parent_path() / "game.tapp")
			.lexically_normal()
			.generic_string();
		args.push_back("--project");
		args.push_back(manifestPath.c_str());
	}
	int exitCode = 0;
	if (!app::ParsePakAppArgs(int(args.size()), args.data(), cfg.cli,
			exitCode))
		return exitCode;

	// ---- THE RUNTIME FAÇADE: stable ids resolve to real bytes -------
	// The compiled header's project::textures::stone (a constexpr id
	// from the project's asset db) resolves through the appmanifest's
	// [ids] table to its pak + path, and the bytes come back through
	// the pak backend — game code never types a path or an index.
	// (Cross-pak by construction: the texture lives in shared.pak while
	// the entry scene rides game.pak.)
	if (!cfg.cli.project.empty()) {
		std::string rtErr;
		if (auto runtime = app::ProjectRuntime::Load(cfg.cli.project,
				rtErr)) {
			// ---- THE COMPILED-CATALOG CONTRACT (the flattened
			// project.h): the game proves ITSELF against the deployed
			// content up front — the header's flat Assets table (the
			// compile-time POD records) + its CatalogFingerprint
			// validate against the .tapp's [ids] rows + `catalog` line.
			// After this, the game KNOWS the ids it was compiled with
			// are the ids the content ships — no runtime discovery, no
			// .tapp row ever reaches gameplay code.
			static_assert(sizeof(project::AssetDesc)
					== sizeof(app::ProjectRuntime::CompiledAssetDesc)
				&& offsetof(project::AssetDesc, id)
					== offsetof(app::ProjectRuntime::CompiledAssetDesc,
						id)
				&& offsetof(project::AssetDesc, pakIndex)
					== offsetof(app::ProjectRuntime::CompiledAssetDesc,
						pakIndex)
				&& offsetof(project::AssetDesc, path)
					== offsetof(app::ProjectRuntime::CompiledAssetDesc,
						path),
				"the generated AssetDesc and the runtime's mirror must"
				" stay layout-identical (member order + types)");
			if (runtime->AttachCompiledCatalog(
					reinterpret_cast<
						const app::ProjectRuntime::CompiledAssetDesc*>(
						project::Assets.data()),
					project::Assets.size(),
					project::CatalogFingerprint, rtErr))
				std::printf("[GameProject] compiled catalog attached +"
					" validated against the deployment manifest"
					" (fingerprint 0x%llx, %zu asset(s))\n",
					(unsigned long long)project::CatalogFingerprint,
					project::Assets.size());
			else
				std::printf("[GameProject] compiled catalog REJECTED:"
					" %s\n", rtErr.c_str());
			// The flat table is a first-class compile-time surface:
			// kind + pakIndex typed queries, no strings.
			if (const project::AssetDesc* tex =
					project::Find(project::textures::stone))
				std::printf("[GameProject] flat table: stone -> kind %u"
					" pak[%u] '%.*s'\n",
					unsigned(tex->kind), tex->pakIndex,
					int(tex->path.size()), tex->path.data());
			if (const smvk::AppManifestAsset* row =
					runtime->Asset(project::textures::stone)) {
				std::printf("[GameProject] id 0x%llx -> %s '%s'\n",
					(unsigned long long)project::textures::stone,
					row->pak.c_str(), row->path.c_str());
				if (auto bytes = runtime->ReadAsset(
						project::textures::stone, rtErr))
					std::printf("[GameProject] read %zu bytes of"
						" ktx2 through %s\n", bytes->size(),
						row->pak.c_str());
				else
					std::printf("[GameProject] texture read failed:"
						" %s\n", rtErr.c_str());
			}
			else {
				std::printf("[GameProject] the id table does not carry"
					" the texture (rebuild GameProject_paks)\n");
			}
		}
	}
	// ---- THE AUDIO LAYER: the scene-aware runtime over the mixer ----
	// The AudioRuntime element stacks over the renderer (the host
	// extension hook): it owns the clip cache (sound ids decoded
	// through the project's pak), the emitter states (the authored
	// audio-source nodes — this scene's BellTower, with its playOnLoad
	// + loop ring on load), and the spatialization (attenuation + pan
	// against the active camera, per frame). Gameplay code drives it
	// through the imperative surface: PrefetchSound /
	// PlaySound(world, node, sound) / StopSound / SetListener.
	if (!cfg.cli.project.empty()) {
		std::string audioTapp = cfg.cli.project;
		cfg.extraElement =
			[audioTapp](smvk::App& host,
				engine::WorldRenderer& renderer,
				std::shared_ptr<smvk::World*> worldHandle)
			-> std::shared_ptr<engine::IAppElement> {
			// The audio runtime + the ProjectLit family demo, composed
			// into ONE element (RunPakApp carries a single extraElement
			// slot; the demo forwards every lifecycle call to the audio
			// leg it wraps).
			std::shared_ptr<smvk::World*> worldForDemo = worldHandle;
			return std::make_shared<ProjectLitDemo>(
				app::MakeAudioRuntime(host, audioTapp,
					std::move(worldHandle), &renderer),
				std::move(worldForDemo));
		};
	}
	return app::RunPakApp(cfg);
}
