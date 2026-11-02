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

#include "Runtime.h"
#include "AudioRuntime.h"

#include "project.h"

#include <Engine/App/world.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

// ---- THE PROJECT LIT FAMILY DEMO (docs/shader_library_design.md
// Phase B) — AS THE GAME -----------------------------------------
// The two worked-example effects — enemy damage flash (worked example
// A) and runtime wetness (worked example B) — shipped as the sample's
// demonstration material: the scene's first opaque material opts into
// the ProjectLit family (the project-authored lit_hooks_project.hlsli
// compiled into project_lit.*.spv) and a per-frame pulse drives both
// userdata bit fields through the family's hooks. No shader recompile,
// no reupload, no batch split — a family switch is a pipeline pick,
// a userdata change is a CPU write.
//
// THE GAME BEHAVIOUR (app::GameBehaviour — the application-facing
// runtime): onStart adopts the material (the world + the session are
// live by then), the update pulses it. The service elements (the
// scene-aware audio) stack UNDER this through the launch options —
// no nested lifecycle-forwarding wrappers anywhere.
class ProjectGame final : public app::GameBehaviour {
public:
	void onStart(app::Context& ctx) override { AdoptMaterial(ctx); }
	void onUpdate(app::Context& ctx, float dt) override {
		Pulse(ctx, dt);
	}

private:
	// Picks the scene's first opaque material and opts it into the
	// family. A missing family (engine-only deployment, no
	// project_lit.*.spv) is NOT an error: the renderer's consult
	// reports the StandardLit fallback loudly and the demo simply has
	// no effect to drive.
	void AdoptMaterial(app::Context& ctx) {
		smvk::World* world = ctx.world();
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
	void Pulse(app::Context& ctx, float dt) {
		smvk::World* world = ctx.world();
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

	// Central run config (the TOURMALINE_* pins): RunGame parses + logs
	// it (the sample's old block, lifted into the runner).

	// ---- THE GAME LAUNCH (app::RunGame — the application boundary) --
	// ONE call: the packaged manifest beside the exe becomes the entry
	// point (unless the caller names one), the session builds before
	// any graphics work, the FATAL compiled-catalog gate proves the
	// game against the deployed content (the typed helper's layout
	// static_asserts at THIS instantiation — the last reinterpret_cast
	// left the tree with it), the SERVICE elements stack under the
	// game element, and the game's own hooks (the ProjectLit demo)
	// carry the gameplay. No viewer config, no manifest rows, no pak
	// filenames in game code.
	app::GameLaunchOptions opts;
	// The service: the scene-aware audio runtime (borrowing the
	// launch's SHARED content session — the same pak images + provider
	// selection the world load resolved through).
	opts.services.push_back(
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
	// The flat table is a first-class compile-time surface: kind-typed
	// queries, no strings (IDENTITY ONLY — the package addressing is
	// selection state, never part of the catalog).
	if (const project::AssetDesc* tex =
			project::Find(project::textures::stone))
		std::printf("[GameProject] flat table: stone -> kind %u"
			" '%.*s'\n",
			unsigned(tex->kind),
			int(tex->path.size()), tex->path.data());
	ProjectGame game;
	return app::RunGame(&game, argc, argv,
		project::Assets.data(), project::Assets.size(),
		project::CatalogFingerprint, std::move(opts));
}
