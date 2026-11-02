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
// A STANDALONE element: the audio runtime stacks as its OWN element
// beside this one (the host's extension list composes elements, never
// forces a game to nest lifecycle-forwarding wrappers).
class ProjectLitDemo final : public engine::IAppElement {
public:
	explicit ProjectLitDemo(
			std::shared_ptr<smvk::World*> worldHandle)
		: m_worldHandle(std::move(worldHandle)) {}

	const char* ElementName() const override { return "GameProjectDemo"; }

	// The demo's lifecycle is narrow: attach adopts the material, the
	// update pulses it. The rest of the element surface rides as
	// no-ops (a standalone element owns ONLY its concerns — no nested
	// element forwarding).
	bool onAttach() override {
		// The host stacks this element AFTER the renderer's attach (the
		// world source ran), so the scene's materials exist here.
		AdoptMaterial();
		return true;
	}
	void onDetach() override {}
	void onResize(smvk::Extent2D) override {}
	void onUpdate(float dt) override { Pulse(dt); }
	void onUIMenu() override {}
	void onUIRender() override {}
	void onPreRender(engine::ElementFrame&) override {}
	void onRender(engine::ElementFrame&) override {}

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
	// project.h regeneration + rebuild. (Symbol-name-agnostic by
	// design: the emitted symbol depends on the asset-db history -
	// a fresh checkout derives "game" where a warm tree carries the
	// collision suffix "game_2".)
	static_assert(project::MatchAssets<project::MeshKey, "^game.*$">()
		.size() == 1);
	static_assert(project::Contains(
		project::MatchAssets<project::MeshKey, "^game.*$">()[0].id));
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

	// ---- THE RUNTIME FAÇADE + THE COMPILED-CATALOG GATE ----------------
	// The typed helper proves the generated AssetDesc's layout against
	// the runtime mirror ONCE (static_asserts — the sample's old
	// hand-rolled reinterpret_cast + offsetof proof is gone), and
	// RunPakApp runs the gate FATAL at bootstrap: the session builds
	// (the manifest adopts into the registry), the game proves itself
	// against the deployed content BEFORE any graphics work, and a
	// refusal refuses the launch (the old log-and-run-anyway is the
	// exact contract this closes).
	cfg.compiledCatalogGate =
		[](app::ProjectRuntime& session, std::string& err) {
		if (app::AttachCompiledCatalog(session, project::Assets.data(),
				project::Assets.size(), project::CatalogFingerprint,
				err)) {
			std::printf("[GameProject] compiled catalog attached +"
				" validated against the deployment manifest"
				" (fingerprint 0x%llx, %zu asset(s))\n",
				(unsigned long long)project::CatalogFingerprint,
				project::Assets.size());
			return true;
		}
		return false;
	};
	// The flat table is a first-class compile-time surface: kind +
	// pakIndex typed queries, no strings.
	if (const project::AssetDesc* tex =
			project::Find(project::textures::stone))
		std::printf("[GameProject] flat table: stone -> kind %u"
			" pak[%u] '%.*s'\n",
			unsigned(tex->kind), tex->pakIndex,
			int(tex->path.size()), tex->path.data());
	// ---- THE GAME ELEMENTS (composed, never nested) ---------------------
	// TWO standalone elements stack over the renderer, in list order:
	// the scene-aware audio runtime (borrowing the launch's SHARED
	// content session — the same pak images + provider selection the
	// world load resolved through) and the ProjectLit family demo (a
	// plain world handle). No lifecycle-forwarding wrapper: the host's
	// extension list composes elements, each owning its own concerns.
	if (!cfg.cli.project.empty()) {
		cfg.extraElements.push_back(
			[](smvk::App& host,
				engine::WorldRenderer& renderer,
				std::shared_ptr<smvk::World*> worldHandle,
				const std::shared_ptr<app::ProjectRuntime>& session)
			-> std::shared_ptr<engine::IAppElement> {
			if (!session)
				return nullptr; // no session (a --pak override): no audio
			return app::MakeAudioRuntime(host, session,
				std::move(worldHandle), &renderer);
		});
		cfg.extraElements.push_back(
			[](smvk::App&,
				engine::WorldRenderer&,
				std::shared_ptr<smvk::World*> worldHandle,
				const std::shared_ptr<app::ProjectRuntime>&)
			-> std::shared_ptr<engine::IAppElement> {
			return std::make_shared<ProjectLitDemo>(
				std::move(worldHandle));
		});
	}
	return app::RunPakApp(cfg);
}
