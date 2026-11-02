// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// PakWorldLoader — see PakWorldLoader.h. The pak-viewer host's world
// loading (moved out of PakApp.cpp: loading, scene preparation, anims
// and static flags are NOT renderer wiring).

#include "PakWorldLoader.h"

#include "ProjectRuntime.h" // the content session (LoadProjectWorld's resolver lane)

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/appmanifest.h> // kCatalogKind* (the resolver's kind byte)
#include <Engine/Asset/cluster_serializer.h> // ListPakScenes (scene discovery)
#include <Engine/Asset/pak.h>
#include <Engine/Profile/profile.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Scene/scene.h>
#include <Engine/Utils/texture_loader.h>
#include <Engine/Vfs/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

namespace app {
	namespace {

		// Pak stem -> scene id (GI bake files resolve gi/<id>.*, check-scene
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

		// THE SKINNING CLIP (the Northlight bone-eval box's input): a
		// loaded .tourAnim clip attaches to the first cluster carrying a
		// skeleton (the knight pak's cluster). Resolution order (the
		// self-contained ladder — the old discovery DEPENDED on loose
		// files / a live demo-dir enumeration):
		//   1. TOURMALINE_ANIM=<vfs path> (the explicit headless pin),
		//   2. the cluster's RECORDED anim refs (scene.animClipRefs —
		//      the cooked asset names its clips; the pak-carrying blobs
		//      resolve through the session's providers),
		//   3. legacy: '<stem>.anim*' beside the pak under demo/ (old
		//      paks that predate the recorded refs — content/demo-style
		//      layouts where the .anim0_* asset sits next to the scene).
		// No clip = the bind-pose palette (the pass still runs, nothing
		// moves - the deformed heap exists, the fetch swap works).
		// Attaches the first loadable clip to a skinned cluster.
		// EVERY byte + every enumeration resolves through the SESSION
		// (the registry's provider union — overrides + paks, the SAME
		// selection every other content read uses): the host overlay
		// is no longer a content source on any runtime path.
		void AttachClips(tour::Cluster& c, const std::string& stem,
			app::ProjectRuntime& session) {
			if (c.HasAnimClip())
				return;
			std::vector<std::string> candidates;
			if (const char* env = std::getenv("TOURMALINE_ANIM"))
				candidates.push_back(env);
			// 2) The recorded refs (the cooked asset's explicit animation
			// references — the pak is self-contained: the refs name the
			// embedded blobs the pak_cooker packed).
			for (const std::string& ref : c.scene.animClipRefs)
				if (!ref.empty())
					candidates.push_back(ref);
			// 3) Legacy demo-dir enumeration (pre-ref paks only) —
			// through the registry's ONE enumeration surface (the
			// provider union under the prefix; the importer names clips
			// '<stem>.anim<idx>_<sanitized glTF animation name>.anim',
			// so enumerate the demo dir for '<stem>.anim*' rather than
			// guessing a label — the union is sorted, so this is stable).
			if (c.scene.animClipRefs.empty()) {
				const std::string prefix = "demo/" + stem + ".anim";
				for (const std::string& e :
						session.Registry().ListPathsUnder("demo")) {
					if (e.size() >= prefix.size() + 5
						&& e.compare(0, prefix.size(), prefix) == 0
						&& e.compare(e.size() - 5, 5, ".anim") == 0)
						candidates.push_back(e);
				}
			}
			for (const std::string& path : candidates) {
				const std::string norm = smvk::NormalizeAssetUri(path).Str();
				std::string rErr;
				auto bytes = session.ReadAssetPath(
					norm.empty() ? path : norm, rErr);
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
		}

	} // namespace

	// The post-load tail SHARED by both entry points (--pak's legacy
	// path and --project): anim-clip attach, cooked static flags, the
	// resolve/fixup, and the env-gated scene-prep diagnostics. EVERY
	// content read resolves through the SESSION (the registry's
	// provider union — the host overlay is no longer a content source
	// on any runtime path).
	void PostLoadTail(tour::World& world, const std::string& pakStem,
		app::ProjectRuntime& session) {
		// THE SKINNING CLIP: attach to the first cluster carrying a
		// skeleton (ONE for now — the driver's cap).
		{
			SMVK_PROFILE_IO("PakWorld_AttachClips");
			for (auto& clusterPtr : world.clusters) {
				tour::Cluster* c = clusterPtr.get();
				if (!c || !c->hasSkeleton)
					continue;
				AttachClips(*c, pakStem, session);
				break;
			}
		}
		// STATIC FLAGS come from the COOKED marks now (the pak's
		// staticRoot bits — authored in the editor/cooker, preserved by
		// the serializer). The old flow blanket-marked every non-skinned
		// cluster root static AT RUNTIME — inventing scene metadata the
		// cooker never wrote, which misclassified any genuinely dynamic
		// (runtime-moved) cluster as frozen. The viewer-side freeze now
		// fires ONLY as the loud compat fallback for clusters whose scene
		// blob carried NO static section (pre-v2 paks).
		{
			// Cooked static flags + the node hierarchy resolve/fixup tail.
			SMVK_PROFILE_IO("PakWorld_Fixup");
			ApplyCookedStaticFlags(world, pakStem);
			world.ResolveAndFixup_clusters();
		}
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
				smvk::Material* m = world.GetMaterial(mi);
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
			for (const MeshRow& row : world.scene.tables.meshes) {
				const uint32_t node = row.node;
				const uint32_t meshIdx = row.mesh;
				if (meshIdx >= world.meshLocations.size())
					continue;
				tour::Cluster* cl = world.meshLocations[meshIdx].cluster;
				if (!cl || !cl->hasSkeleton)
					continue;
				const glm::mat4& g = world.scene.globalTransform[node];
				std::cerr << "[KD] merged meshNode=" << node
					<< " t=(" << g[3].x << "," << g[3].y << "," << g[3].z
					<< ") sc=(" << glm::length(glm::vec3(g[0])) << ","
					<< glm::length(glm::vec3(g[1])) << ","
					<< glm::length(glm::vec3(g[2])) << ")\n";
				if (++n > 8) break;
			}
			std::cerr << "[KD] skinned draw nodes: " << n << "\n";
		}
	}

	// STATIC-FLAG APPLICATION (public seam: PakHostTests drives it on
	// hand-built value worlds — the loader's decision logic without the
	// pak/GPU plumbing). Cooked detection is SECTION PRESENCE
	// (Scene::staticRootsSerialized — set by the cluster deserializer when
	// the scene blob carried the trailing static section), NEVER
	// any_of(staticRoot): a serialized ALL-ZERO array is authored
	// all-dynamic data and must be honored — the any_of form misread
	// exactly that as a legacy pak and re-invented the viewer-side freeze
	// (the blanket marking this pipeline removed). The fallback stays for
	// genuinely legacy clusters (a pre-v2 blob / hand-built scene) and is
	// loud; skinned clusters never freeze (the animation driver owns
	// their transforms).
	void ApplyCookedStaticFlags(tour::World& world, const std::string& pakName) {
		for (auto& clusterPtr : world.clusters) {
			tour::Cluster* c = clusterPtr.get();
			if (!c || c->scene.hierarchy.empty())
				continue;
			if (c->scene.staticRootsSerialized)
				continue; // cooked: honor the recorded marks verbatim
				          // (all-zero = authored all-dynamic)
			if (c->hasSkeleton)
				continue; // skinned clusters stay dynamic (the anim driver)
			std::cerr << "[App] cluster '" << pakName
				<< "' carries no serialized static section (a legacy pak);"
				" freezing its roots viewer-side — recook for authored"
				" static flags\n";
			MarkSubtreeStatic(c->scene, 0);
		}
	}

	tour::mem::LinearPtr<tour::World> LoadPakWorld(
		PakPreload pre,
		int sceneIndex,
		smvk::ResourceManager& ctx,
		smvk::OverlayVfs& contentVfs,
		tour::TextureStreamer* sceneStreamer,
		app::ProjectRuntime& session
	) {
		// Optick: the synchronous scene-load path (startup / scene-switch
		// latency, not steady frame cost) — stage scopes below.
		SMVK_PROFILE_IO("PakWorld_Load");
		if (!pre.backend) {
			std::cerr << "[App] pak preload failed: " << pre.error << "\n";
			return nullptr;
		}
		std::cerr << "[App] loading pak '" << pre.scenePak << "'\n";
		const smvk::PakReader& reader = pre.backend->Reader();
		std::cerr << "[App] pak parsed (" << reader.EntryCount() << " entries)\n";
		// Scene discovery: list, default to index 0.
		std::string lerr;
		auto scenes = [&] {
			SMVK_PROFILE_IO("PakWorld_DiscoverScenes");
			return smvk::ListPakScenes(reader, &lerr);
		}();
		SMVK_PROFILE_TAG("scenes", scenes.size());
		if (scenes.empty()) {
			// Terrain-only pak (no scenes, Terrain blobs present): an empty
			// world carrying just the harvested terrain. The renderer bakes
			// G-blocks from it; the camera needs explicit TOURMALINE_CAM_*
			// pins (empty-scene framing has no bounds to fit).
			if (!reader.BlobsOfKind(smvk::PakAssetKind::Terrain).empty()) {
				std::cerr << "[App] no scenes: terrain-only pak, loading "
					"terrain without a scene\n";
				auto world = tour::mem::MakeProcess<tour::World>();
				std::string terrErr;
				if (!world->LoadTerrainFromPak(reader, terrErr)) {
					std::cerr << "[App] terrain harvest failed: " << terrErr
						<< "\n";
					return nullptr;
				}
				world->ResolveAndFixup_clusters();
				contentVfs.Mount(pre.backend, 10, pre.scenePak);
				// The shared-image lane: the backend adopts into the
				// session's registry too (every later read rides the same
				// image + the same provider selection).
				session.AdoptPakBackend(pre.scenePak, pre.backend);
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
		auto world = tour::mem::MakeProcess<tour::World>();
		{
			// The cluster deserialize + GPU uploads (meshes, materials,
			// textures staged through the streamer) — the dominant
			// scene-switch cost.
			SMVK_PROFILE_IO("PakWorld_LoadCluster");
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
		}
		std::cerr << "[App] pak cluster loaded\n";
		// Mount the pak backend into the overlay (the world-source
		// protocol role: the engine hands the callback a vfs, hosts
		// mount their paks) AND ADOPT it into the session's registry —
		// the shared-image lane. The anim resolution + every later
		// read ride the session's provider selection; the overlay is
		// no longer a content source on any runtime path.
		contentVfs.Mount(pre.backend, 10, pre.scenePak);
		session.AdoptPakBackend(pre.scenePak, pre.backend);
		PostLoadTail(*world, StemOf(pre.scenePak), session);
		return world;
	}

	tour::mem::LinearPtr<tour::World> LoadProjectWorld(
		std::vector<PakPreload> preloads,
		size_t entryPakIndex,
		const std::string& entrySceneName,
		smvk::ResourceManager& ctx,
		smvk::OverlayVfs& contentVfs,
		tour::TextureStreamer* sceneStreamer,
		app::ProjectRuntime* session
	) {
		SMVK_PROFILE_IO("ProjectWorld_Load");
		// EVERY listed pak must have preloaded clean — one missing or
		// corrupt archive fails the launch, NAMED, before anything loads
		// (never an unrelated fallback scene).
		for (const PakPreload& p : preloads) {
			if (!p.backend) {
				std::cerr << "[App] project pak failed: " << p.scenePak
					<< " (" << p.error << ")\n";
				return nullptr;
			}
		}
		if (entryPakIndex >= preloads.size()) {
			std::cerr << "[App] project entry pak index out of range\n";
			return nullptr;
		}
		// Mount ALL the backends FIRST (manifest order) — BEFORE the
		// cluster load — into the overlay (the loose/legacy anim lane
		// still reads there) AND, when the launch carries a session,
		// ADOPT them into its registry: the shared-image lane — every
		// later byte read (the entry load's cross-pak resolver below,
		// the audio element's sound decodes, game ReadAsset calls)
		// serves through the same images + the same provider selection.
		for (const PakPreload& p : preloads) {
			contentVfs.Mount(p.backend, 10, p.scenePak);
			if (session)
				session->AdoptPakBackend(p.scenePak, p.backend);
		}

		const PakPreload& entry = preloads[entryPakIndex];
		const smvk::PakReader& reader = entry.backend->Reader();
		std::cerr << "[App] project: entry pak '" << entry.scenePak
			<< "' (" << reader.EntryCount() << " entries, "
			<< preloads.size() << " pak(s) mounted)\n";
		// Resolve the entry scene BY IDENTITY: the manifest's scene name
		// against the pak's scene list — never index 0 by default, never
		// a fallback (a name mismatch is a build/runtime version skew
		// and fails loudly).
		std::string lerr;
		const std::vector<smvk::PakSceneInfo> scenes =
			smvk::ListPakScenes(reader, &lerr);
		if (scenes.empty()) {
			std::cerr << "[App] project: the entry pak carries no scenes: "
				<< lerr << "\n";
			return nullptr;
		}
		size_t entryIdx = scenes.size();
		for (size_t i = 0; i < scenes.size(); ++i)
			if (scenes[i].name == entrySceneName) {
				entryIdx = i;
				break;
			}
		if (entryIdx == scenes.size()) {
			std::cerr << "[App] project: the entry scene '" << entrySceneName
				<< "' is not in the entry pak (scenes:";
			for (const smvk::PakSceneInfo& s : scenes)
				std::cerr << " '" << (s.name.empty() ? "<unnamed>" : s.name)
					<< "'";
			std::cerr << ") — recook the project\n";
			return nullptr;
		}
		std::cerr << "[App] project: loading entry scene[" << entryIdx
			<< "] '" << scenes[entryIdx].name << "'\n";

		// The cross-pak texture resolver (DeserializeCluster's external
		// hook): a material URI with no blob in the entry pak is served
		// from whatever provider carries its logical path — through the
		// SESSION's registry (the ONE resolution authority: an override
		// serving the path wins here exactly as it wins for the
		// compiled-id form — the convergence contract; the overlay is
		// no longer a content source on any runtime path). The bytes
		// are the embedded .ktx2, the standard parallel loader decodes
		// + uploads, and the texture registers on the cluster under
		// its original URI. v1 note: the cross-pak path takes the
		// DEFAULT sampler (the pak-served path's payload filters stay
		// with the owning pak's own loads).
		smvk::ExternalTextureResolver resolver =
			[&ctx, session](tour::Cluster& c,
				const std::string& uri) -> smvk::TextureHandle {
			const std::string norm = smvk::NormalizeAssetUri(uri).Str();
			const std::string probe = norm.empty() ? uri : norm;
			std::string rErr;
			std::optional<std::vector<uint8_t>> bytes =
				session->ReadAssetByPath(
					smvk::kCatalogKindTexture, probe, rErr);
			if (!bytes || bytes->empty()) {
				std::cerr << "[App] cross-pak texture unresolved: " << uri
					<< "\n";
				return {};
			}
			std::vector<smvk::KtxMemoryJob> jobs;
			jobs.push_back(smvk::KtxMemoryJob{
				0, bytes->data(), bytes->size(), uri });
			smvk::ParallelTextureStats stats;
			auto holders = smvk::LoadKtxTexturesParallelMemory(ctx, jobs,
				{}, &stats);
			const auto it = holders.find(0);
			if (it == holders.end()) {
				std::cerr << "[App] cross-pak texture decode failed: "
					<< uri << "\n";
				return {};
			}
			const smvk::TextureHandle th = it->second;
			c.textures.create(std::move(it->second));
			c.RegisterTextureUri(uri, th);
			return th;
		};

		auto world = tour::mem::MakeProcess<tour::World>();
		{
			SMVK_PROFILE_IO("ProjectWorld_LoadCluster");
			try {
				world->LoadClusterFromPak(ctx, reader,
					uint32_t(entryIdx), sceneStreamer, entry.image,
					&resolver);
			}
			catch (const std::exception& e) {
				std::cerr << "[App] project pak load failed: " << e.what()
					<< "\n";
				ctx.WaitForUploads();
				return nullptr;
			}
		}
		std::cerr << "[App] project cluster loaded\n";
		PostLoadTail(*world, StemOf(entry.scenePak), *session);
		return world;
	}

} // namespace app
