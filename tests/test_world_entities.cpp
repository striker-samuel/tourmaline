// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// world_entity_tests: cluster unload + dynamic-cluster contracts.
// GPU-less: clusters are built as pure value worlds (scene nodes + light
// pools — no uploads, ctx nullptr); resolve/unload never touch the GPU.
//
// Hand-rolled runner (same conventions as test_assets): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/App/animator.h>
#include <Engine/Asset/touranim.h>
#include <Engine/Scene/scene.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << std::endl;
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << std::endl;
	}
}

// A point light + a root node carrying it, both in the CLUSTER's own scene
// (the honest route: a later ResolveAndFixup folds them into the merged
// world — the cluster-scene light table stores the CLUSTER-LOCAL pool
// index, the resolve remap rewrites it to the world lightLocations index).
smvk::ClusterLightHandle AddLight(smvk::Cluster& cl, const char* name,
	float intensity) {
	::SceneLight def{};
	def.type = ::SceneLight::Type::Point;
	def.intensity = intensity;
	def.name = name;
	auto handle = cl.CreateLight(def);
	const int node = addNode(cl.scene, -1, 0);
	SetLight(cl.scene, uint32_t(node), handle.index());
	return handle;
}

// True when any world light location points into `cluster`.
bool WorldHasLightFrom(const smvk::World& w, const smvk::Cluster* cluster) {
	for (const auto& loc : w.lightLocations)
		if (loc.cluster == cluster)
			return true;
	return false;
}

void TestUnloadAndDynamicCluster() {
	smvk::World w;

	// Two "pak" clusters, each carrying one light.
	auto a = std::make_unique<smvk::Cluster>(nullptr);
	smvk::Cluster* ap = a.get();
	AddLight(*a, "a_light", 100.0f);
	w.RegisterCluster(std::move(a));
	auto b = std::make_unique<smvk::Cluster>(nullptr);
	smvk::Cluster* bp = b.get();
	AddLight(*b, "b_light", 200.0f);
	w.RegisterCluster(std::move(b));

	// A runtime-created entity in the dynamic cluster.
	smvk::Cluster& dyn = w.EnsureDynamicCluster(nullptr);
	Check(dyn.isDynamic, "dyn.flag");
	AddLight(dyn, "dyn_light", 300.0f);

	// Idempotent: the same cluster comes back.
	Check(&w.EnsureDynamicCluster(nullptr) == &dyn, "dyn.idempotent");
	Check(w.clusters.size() == 3, "dyn.count");

	w.ResolveAndFixup_clusters();
	Check(w.lightLocations.size() == 3, "resolve.lights");
	Check(WorldHasLightFrom(w, ap) && WorldHasLightFrom(w, bp)
		&& WorldHasLightFrom(w, &dyn), "resolve.clusters");

	// Unload one pak cluster: its entities vanish, the rest survive.
	w.UnloadCluster(*ap);
	Check(w.clusters.size() == 2, "unload.count");
	Check(w.lightLocations.size() == 2, "unload.lights");
	Check(!WorldHasLightFrom(w, ap), "unload.a.gone");
	Check(WorldHasLightFrom(w, bp) && WorldHasLightFrom(w, &dyn),
		"unload.survivors");

	// The merged scene no longer links any node to a light of the unloaded
	// cluster: every light-table row resolves into the surviving table.
	bool linksOk = true;
	for (const LightRow& row : w.scene.tables.lights)
		linksOk = linksOk && row.light < w.lightLocations.size();
	Check(linksOk, "unload.links.remap");

	// Unloading the dynamic cluster is refused (runtime state).
	w.UnloadCluster(dyn);
	Check(w.clusters.size() == 2, "unload.dyn.refused");
	Check(w.lightLocations.size() == 2, "unload.dyn.lights.intact");

	// Unregister detaches the other pak cluster (alive, re-parentable) and
	// rebuilds over the survivors: only the dynamic light remains.
	auto detached = w.UnregisterCluster(*bp);
	Check(detached != nullptr && detached.get() == bp
		&& detached->world == nullptr, "unregister.detached");
	Check(w.clusters.size() == 1, "unregister.count");
	Check(w.lightLocations.size() == 1
		&& WorldHasLightFrom(w, &dyn), "unregister.lights");

	// Unregistering the dynamic cluster is refused too.
	Check(w.UnregisterCluster(dyn) == nullptr, "unregister.dyn.refused");
	Check(w.clusters.size() == 1, "unregister.dyn.count.intact");
}

// Runtime audio-source entities (the gameplay-handle surface —
// World::SpawnAudioSource/DestroyAudioSource + the AudioSourceHandle
// registry; GPU-less, headless): spawn → rows + nodes + a live
// handle; resolve → the handle STAYS, the internal node remaps
// (the entity-model contract); destroy → the generation bump
// invalidates every stale copy; the rows drop.
void TestAudioSourceEntities() {
	smvk::World w;

	::SceneAudioSource def{};
	def.volume = 0.8f;
	def.minDistance = 2.0f;
	def.playOnLoad = true;
	def.sounds.push_back(AudioSoundRef{ "Bells",
		"content/audio/bells.wav", 0x4000, 1.0f, true });

	const glm::mat4 frame = glm::translate(glm::mat4(1.0f),
		glm::vec3(3.0f, 4.0f, 5.0f));
	smvk::AudioSourceHandle h = w.SpawnAudioSource(frame,
		UINT32_MAX, def);
	Check(bool(h) && h.Index() == 0 && h.Generation() == 1,
		"audio.spawn-handle");
	Check(w.IsAudioSourceAlive(h), "audio.spawn-alive");
	// The durable row (dynamic cluster's own scene) + the merged
	// mirror both carry the payload.
	smvk::Cluster& dyn = w.EnsureDynamicCluster(nullptr);
	Check(dyn.scene.tables.audioSources.size() == 1
		&& w.scene.tables.audioSources.size() == 1,
		"audio.spawn-rows");
	const uint32_t mergedNode = w.AudioSourceNode(h);
	Check(mergedNode != UINT32_MAX
		&& w.scene.tables.audioSources[0].source.volume == 0.8f,
		"audio.spawn-mirror-payload");

	// A second spawn reuses nothing yet; destroying both then spawning
	// reuses the freed slot with a BUMPED generation.
	smvk::AudioSourceHandle h2 =
		w.SpawnAudioSource(glm::mat4(1.0f), UINT32_MAX, def);
	Check(bool(h2) && h2 != h, "audio.second-spawn");

	// Resolve: the durable rows fold in; the HANDLE stays valid and
	// its merged node REMAPS (ids moved).
	w.ResolveAndFixup_clusters();
	Check(w.IsAudioSourceAlive(h) && w.IsAudioSourceAlive(h2),
		"audio.resolve-handles-stay");
	Check(w.scene.tables.audioSources.size() == 2,
		"audio.resolve-folds-rows");
	{
		const uint32_t remapped = w.AudioSourceNode(h);
		Check(remapped != UINT32_MAX, "audio.resolve-node-remaps");
		const AudioSourceRow* row =
			FindAudioSource(w.scene, remapped);
		Check(row && row->source.playOnLoad
			&& row->source.sounds.size() == 1
			&& row->source.sounds[0].soundId == 0x4000,
			"audio.resolve-row-payload");
		// The world POSE survived the fold (the frame's translate).
		const glm::vec3 t(w.scene.globalTransform[remapped][3]);
		Check(std::abs(t.x - 3.0f) < 1e-4f
			&& std::abs(t.y - 4.0f) < 1e-4f
			&& std::abs(t.z - 5.0f) < 1e-4f,
			"audio.resolve-pose");
	}

	// Destroy h: its rows drop, the generation advances — h is dead,
	// h2 (a different slot) lives.
	const uint32_t hGen = h.Generation();
	Check(w.DestroyAudioSource(h), "audio.destroy");
	Check(!w.IsAudioSourceAlive(h)
		&& !w.DestroyAudioSource(h), // stale destroy refuses
		"audio.destroy-stale-invalid");
	Check(w.IsAudioSourceAlive(h2), "audio.destroy-sibling-lives");
	Check(w.scene.tables.audioSources.size() == 1,
		"audio.destroy-rows-drop");
	// The freed slot re-spawns with the bumped generation.
	smvk::AudioSourceHandle h3 =
		w.SpawnAudioSource(glm::mat4(1.0f), UINT32_MAX, def);
	Check(h3.Index() == h.Index() && h3.Generation() == hGen + 1,
		"audio.slot-reuse-bumps-generation");
	Check(w.IsAudioSourceAlive(h3) && !w.IsAudioSourceAlive(h),
		"audio.slot-reuse-stale-stays-dead");
	// Foreign/garbage handles never resolve.
	Check(!w.IsAudioSourceAlive(smvk::AudioSourceHandle{})
		&& !w.DestroyAudioSource(smvk::MakeEntityHandle<
			smvk::AudioSourceTag>(9999u, 1u)),
		"audio.garbage-handle-refused");
}

// Runtime MESH entities + the ANIMATOR component (the gameplay-handle
// surface — World::SpawnMeshFromNode/DestroyMesh + AddAnimator and the
// playback engine; GPU-less, headless). Pins: the instancing rows +
// the resolve RE-MIRROR (the handle stays, the binding re-establishes),
// the animator's rest capture + track flush into the target cluster
// (poseExternallyDriven), the playback controls (pause/resume/speed/
// wrap), and the attachment cleanup (DestroyMesh kills the animator).
void TestMeshEntitiesAndAnimators() {
	using namespace smvk;
	World w;

	// A "pak" cluster: root(0) + a joint child(1) + a mesh node(2),
	// with a 2-joint skeleton (the v1 single-skeleton contract).
	auto a = std::make_unique<smvk::Cluster>(nullptr);
	a->SetOfflineMode(true);
	Cluster* ap = a.get();
	addNode(a->scene, -1, 0);                          // 0: root
	const int jointChild = addNode(a->scene, 0, 1);    // 1
	const int meshNode = addNode(a->scene, 0, 1);      // 2
	a->skeleton.joints = { 0u, uint32_t(jointChild) };
	a->skeleton.inverseBind = { glm::mat4(1.0f),
		glm::mat4(1.0f) };
	a->skeleton.root = 0;
	a->hasSkeleton = true;
	const auto mh = a->staticMeshes.create(StaticMesh{});
	SetMesh(a->scene, NodeId(meshNode), mh.index());
	Material matDef{};
	matDef.rebuildPipeline();
	const auto math = a->materials.create(std::move(matDef));
	SetMaterial(a->scene, NodeId(meshNode), math.index());
	w.RegisterCluster(std::move(a));
	w.ResolveAndFixup_clusters();

	// The source node (the merged-scene node carrying the mesh).
	uint32_t sourceNode = UINT32_MAX;
	uint32_t sourceMeshIdx = UINT32_MAX;
	for (const MeshRow& row : w.scene.tables.meshes) {
		if (row.mesh < w.meshLocations.size()
				&& w.meshLocations[row.mesh].cluster == ap) {
			sourceNode = row.node;
			sourceMeshIdx = row.mesh;
			break;
		}
	}
	Check(sourceNode != UINT32_MAX, "meshent.source-found");

	// Spawn: the entity node + the CROSS-CLUSTER binding (the source
	// cluster owns the mesh; the mirror rows resolve through the world
	// tables).
	const glm::mat4 spawnFrame = glm::translate(glm::mat4(1.0f),
		glm::vec3(10.0f, 0.0f, 0.0f));
	MeshEntityHandle h = w.SpawnMeshFromNode(sourceNode, spawnFrame,
		UINT32_MAX);
	Check(bool(h) && w.IsMeshEntityAlive(h), "meshent.spawn-alive");
	const uint32_t entNode = w.MeshEntityNode(h);
	Check(entNode != UINT32_MAX
		&& FindMesh(w.scene, entNode) != nullptr
		&& FindMesh(w.scene, entNode)->mesh == sourceMeshIdx,
		"meshent.spawn-mirror-row");
	Check(FindMaterial(w.scene, entNode) != nullptr,
		"meshent.spawn-material-row");
	Check(w.MeshEntityMeshIndex(h) == sourceMeshIdx,
		"meshent.mesh-index-resolves");

	// Resolve: the mirror dies, the binding RE-ESTABLISHES on the
	// remapped node (the handle never moved).
	w.ResolveAndFixup_clusters();
	Check(w.IsMeshEntityAlive(h), "meshent.resolve-handle-stays");
	{
		const uint32_t remapped = w.MeshEntityNode(h);
		Check(remapped != UINT32_MAX
			&& FindMesh(w.scene, remapped) != nullptr
			&& FindMesh(w.scene, remapped)->mesh
				== w.MeshEntityMeshIndex(h),
			"meshent.resolve-remirror");
	}

	// The animator: a 1s translate clip on the child joint (0 -> y=2).
	touranim::Clip clip;
	clip.name = "rise";
	clip.duration = 1.0;
	touranim::Track track;
	track.joint = uint32_t(jointChild);
	track.channel = touranim::Channel::Translate;
	track.spline = touranim::Spline::Linear;
	track.keys = { touranim::Key{ 0.0, glm::vec4(0, 0, 0, 0) },
		touranim::Key{ 1.0, glm::vec4(0, 2, 0, 0) } };
	clip.tracks.push_back(track);
	Animator anim = w.AddAnimator(h, clip);
	Check(bool(anim) && anim.Playing(), "anim.add-ok");

	// The playback engine: Update advances + flushes the pose into the
	// TARGET cluster's scene (the first animator claims it).
	w.Update(0.5f);
	{
		const glm::vec3 t(ap->scene.localTransform[jointChild][3]);
		Check(std::abs(t.y - 1.0f) < 1e-4f, "anim.flush-sample");
		Check(ap->poseExternallyDriven, "anim.cluster-claimed");
		// The untracked joint keeps the REST pose.
		const glm::vec3 t0(ap->scene.localTransform[0][3]);
		Check(std::abs(t0.y) < 1e-5f, "anim.rest-untracked");
	}
	// Pause freezes; resume continues.
	anim.Pause();
	w.Update(0.25f);
	{
		const glm::vec3 t(ap->scene.localTransform[jointChild][3]);
		Check(std::abs(t.y - 1.0f) < 1e-4f, "anim.pause-freezes");
	}
	anim.Resume();
	w.Update(0.25f);
	{
		const glm::vec3 t(ap->scene.localTransform[jointChild][3]);
		Check(std::abs(t.y - 1.5f) < 1e-4f, "anim.resume-continues");
	}
	// Speed 2 + 0.25s: the cursor moves 0.75 -> 1.25 -> the LOOP wraps
	// once to 0.25 -> y = 0.5.
	anim.SetSpeed(2.0f);
	w.Update(0.25f);
	{
		const glm::vec3 t(ap->scene.localTransform[jointChild][3]);
		Check(std::abs(t.y - 0.5f) < 1e-4f, "anim.speed-loop-wrap");
	}
	Check(std::abs(anim.Speed() - 2.0f) < 1e-5f, "anim.speed-reads");

	// CrossFade (back to speed 1): the current (rise @ 0.25, heading
	// to y=2) fades against a sink clip (y 0 -> -2) over 1s. At the
	// fade midpoint: rise has advanced to 0.75 (y=1.5), sink sits at
	// its 0.5 point (y=-1), the 0.5/0.5 blend -> y = 0.25.
	anim.SetSpeed(1.0f);
	touranim::Clip sink;
	sink.name = "sink";
	sink.duration = 1.0;
	touranim::Track sinkTrack;
	sinkTrack.joint = uint32_t(jointChild);
	sinkTrack.channel = touranim::Channel::Translate;
	sinkTrack.spline = touranim::Spline::Linear;
	sinkTrack.keys = { touranim::Key{ 0.0, glm::vec4(0, 0, 0, 0) },
		touranim::Key{ 1.0, glm::vec4(0, -2, 0, 0) } };
	sink.tracks.push_back(sinkTrack);
	anim.CrossFade(sink, 1.0f);
	w.Update(0.5f);
	{
		const glm::vec3 t(ap->scene.localTransform[jointChild][3]);
		Check(std::abs(t.y - 0.25f) < 1e-3f, "anim.crossfade-blend");
	}

	// DestroyMesh: the ATTACHED animator dies with the entity (the
	// Kill() contract) + the cluster's claim clears.
	const AnimatorHandle animHandle = anim.handle();
	Check(w.DestroyMesh(h), "meshent.destroy");
	Check(!w.IsMeshEntityAlive(h) && !w.DestroyMesh(h),
		"meshent.destroy-stale-invalid");
	Check(!w.IsAnimatorAlive(animHandle), "anim.dies-with-mesh");
	Check(!ap->poseExternallyDriven, "anim.claim-cleared");
	Check(FindMesh(w.scene, w.MeshEntityNode(h)) == nullptr,
		"meshent.rows-dropped");
	// The stale facade no-ops (never crashes).
	anim.Pause();
	anim.SetSpeed(4.0f);

	// A static cluster (no skeleton) refuses the animator.
	{
		World w2;
		auto s = std::make_unique<smvk::Cluster>(nullptr);
		s->SetOfflineMode(true);
		addNode(s->scene, -1, 0);
		const auto smh = s->staticMeshes.create(StaticMesh{});
		SetMesh(s->scene, 0, smh.index());
		w2.RegisterCluster(std::move(s));
		w2.ResolveAndFixup_clusters();
		uint32_t src = UINT32_MAX;
		for (const MeshRow& row : w2.scene.tables.meshes)
			src = row.node;
		MeshEntityHandle sh = w2.SpawnMeshFromNode(src,
			glm::mat4(1.0f), UINT32_MAX);
		Check(bool(sh), "anim.static-spawn");
		Animator refused = w2.AddAnimator(sh, clip);
		Check(!bool(refused), "anim.static-refused");
	}
}

void TestDecalEntities() {
	smvk::World w;

	// One pak cluster carrying a COOKED decal (node + pool def in the
	// cluster's own scene — the resolve-foldable route).
	auto a = std::make_unique<smvk::Cluster>(nullptr);
	smvk::Cluster* ap = a.get();
	{
		::SceneDecal def{};
		def.albedoUri = "decals/blood_albedo";
		def.normalUri = "decals/blood_normal";
		def.opacity = 0.9f;
		def.seed = 11u;
		def.name = "cooked_splat";
		const auto h = a->CreateDecal(std::move(def));
		const int node = addNode(a->scene, -1, 0);
		SetDecal(a->scene, uint32_t(node), h.index());
	}
	w.RegisterCluster(std::move(a));
	w.ResolveAndFixup_clusters();
	Check(w.decalLocations.size() == 1, "decal.resolve.count");
	Check(w.decalLocations[0].cluster == ap
		&& w.decalLocations[0].handle.valid(), "decal.resolve.location");

	// Runtime creation (the app-preset / J-spawn route): node + def land
	// in the DYNAMIC cluster, linked incrementally (no resolve).
	const glm::mat4 frame = ::DecalNodeFrame(
		glm::vec3(1.0f, 0.5f, 2.0f), glm::vec3(0, -1, 0), glm::vec3(0, 1, 0),
		glm::vec3(0.5f, 0.5f, 0.75f));
	::SceneDecal rt{};
	rt.albedoUri = "decals/blood_albedo";
	rt.seed = 23u;
	rt.name = "runtime_splat";
	const NodeId node = w.CreateDecalAt(frame, std::move(rt));
	Check(node != UINT32_MAX, "decal.create.node");
	Check(w.decalLocations.size() == 2, "decal.create.count");
	const DecalRow* drow = FindDecal(w.scene, node);
	Check(drow != nullptr, "decal.create.link");
	Check(drow != nullptr
		&& w.decalLocations[drow->decal].cluster != nullptr
		&& w.decalLocations[drow->decal].cluster->isDynamic,
		"decal.create.dynamic");
	// The frame IS the node transform (CreateDecalAt contract).
	Check(w.scene.localTransform[node] == frame, "decal.create.frame");

	// Shape round-trip from the node transform: center from the
	// translation, half extents from the column lengths.
	const glm::mat4& g = w.scene.globalTransform[node];
	Check(glm::length(glm::vec3(g[3]) - glm::vec3(1.0f, 0.5f, 2.0f))
		< 1e-5f, "decal.frame.center");
	Check(std::abs(glm::length(glm::vec3(g[0])) - 0.5f) < 1e-5f
		&& std::abs(glm::length(glm::vec3(g[1])) - 0.5f) < 1e-5f
		&& std::abs(glm::length(glm::vec3(g[2])) - 0.75f) < 1e-5f,
		"decal.frame.half");

	// Unload the pak cluster: its COOKED decal vanishes, while the
	// runtime entity FOLDS BACK IN from the dynamic cluster's own scene
	// (its durable home — see CreateDecalAt) with a fresh world node id
	// and a rebuilt link.
	w.UnloadCluster(*ap);
	Check(w.decalLocations.size() == 1, "decal.unload.count");
	Check(w.decalLocations[0].cluster->isDynamic, "decal.unload.survivor");
	bool foldedLink = false;
	for (const DecalRow& row : w.scene.tables.decals)
		foldedLink = foldedLink || row.decal < w.decalLocations.size();
	Check(foldedLink, "decal.unload.folded.link");
	// The folded node carries the same placement frame (cluster-scene
	// original, not the merged-scene mirror).
	for (const DecalRow& row : w.scene.tables.decals) {
		const glm::mat4& fg = w.scene.globalTransform[row.node];
		Check(glm::length(glm::vec3(fg[3]) - glm::vec3(1.0f, 0.5f, 2.0f))
			< 1e-5f, "decal.unload.folded.frame");
		break;
	}
}

// Bake-payload channel contract (the GI/cubemap content home): dynamic
// cluster first (runtime quick-bakes override pak payloads), then pak
// clusters in registration order; cluster unload drops its payloads.
void TestBakePayloadChannel() {
	smvk::World w;
	auto a = std::make_unique<smvk::Cluster>(nullptr);
	smvk::Cluster* ap = a.get();
	a->AddBakePayload("gi/scene.sggi", { 1, 2, 3, 4 });
	a->AddBakePayload("gi/scene_0.sgrv", { 5 });
	w.RegisterCluster(std::move(a));

	// Pak payload resolves.
	auto s = w.ReadBakeBytes("gi/scene_0.sgrv");
	Check(s.size() == 1 && s[0] == 5, "bake.pak.read");

	// Runtime injection on the DYNAMIC cluster overrides the pak payload
	// at the same path and adds new ones.
	smvk::Cluster& dyn = w.EnsureDynamicCluster(nullptr);
	dyn.AddBakePayload("gi/scene.sggi", { 9, 9 });
	dyn.AddBakePayload("gi/quick.sgrv", { 7 });
	auto over = w.ReadBakeBytes("gi/scene.sggi");
	Check(over.size() == 2 && over[0] == 9, "bake.dynamic.override");
	auto add = w.ReadBakeBytes("gi/quick.sgrv");
	Check(add.size() == 1 && add[0] == 7, "bake.inject");
	Check(w.ReadBakeBytes("gi/missing").empty(), "bake.miss");

	// Unloading the pak cluster drops its payloads; the dynamic
	// injections survive.
	w.UnloadCluster(*ap);
	auto kept = w.ReadBakeBytes("gi/scene.sggi");
	Check(kept.size() == 2 && kept[0] == 9, "bake.unload.dyn.survives");
	Check(w.ReadBakeBytes("gi/scene_0.sgrv").empty(), "bake.unload.pak.dropped");
}

// Env-zone entities (see ZoneCovers/ZoneAtPosition): coverage from the
// node transform (scale = half extents, INFINITE components = unbounded
// slabs), resolution by priority descending then innermost, zoneForNode
// resolving a node's world origin.
void TestEnvZones() {
	::Scene scene;
	// Zone A: a big box at the origin, half extents (10, 10, 10).
	NodeId zoneA = UINT32_MAX;
	{
		const int n = addNode(scene, -1, 0);
		glm::mat4 m(1.0f);
		m[0] = glm::vec4(10.0f, 0.0f, 0.0f, 0.0f);
		m[1] = glm::vec4(0.0f, 10.0f, 0.0f, 0.0f);
		m[2] = glm::vec4(0.0f, 0.0f, 10.0f, 0.0f);
		scene.localTransform[size_t(n)] = m;
		::SceneEnv envA{};
		SetEnv(scene, uint32_t(n), envA);
		zoneA = uint32_t(n);
		markAsChanged(scene, n);
	}
	recalculateGlobalTransforms(scene);
	Check(::ZoneCovers(scene, zoneA, glm::vec3(5.0f, 5.0f, 5.0f)),
		"zone.cover.inside");
	Check(!::ZoneCovers(scene, zoneA, glm::vec3(10.5f, 0.0f, 0.0f)),
		"zone.cover.outside");

	// Zone B: a SMALL box at the origin, inside A (same priority ->
	// innermost wins at the shared region; C never covers it).
	NodeId zoneB = UINT32_MAX;
	{
		const int n = addNode(scene, -1, 0);
		glm::mat4 m(1.0f);
		m[0] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
		m[1] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
		m[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
		m[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		scene.localTransform[size_t(n)] = m;
		::SceneEnv envB{};
		SetEnv(scene, uint32_t(n), envB);
		zoneB = uint32_t(n);
		markAsChanged(scene, n);
	}
	recalculateGlobalTransforms(scene);

	// Zone C: overlaps A but NOT B (center (5,0,0), half 2), PRIORITY 5
	// -> wins wherever it covers.
	NodeId zoneC = UINT32_MAX;
	{
		const int n = addNode(scene, -1, 0);
		glm::mat4 m(1.0f);
		m[0] = glm::vec4(2.0f, 0.0f, 0.0f, 0.0f);
		m[1] = glm::vec4(0.0f, 2.0f, 0.0f, 0.0f);
		m[2] = glm::vec4(0.0f, 0.0f, 2.0f, 0.0f);
		m[3] = glm::vec4(5.0f, 0.0f, 0.0f, 1.0f);
		scene.localTransform[size_t(n)] = m;
		::SceneEnv envC{};
		envC.zonePriority = 5.0f;
		SetEnv(scene, uint32_t(n), envC);
		zoneC = uint32_t(n);
		markAsChanged(scene, n);
	}
	recalculateGlobalTransforms(scene);

	Check(::ZoneAtPosition(scene, glm::vec3(0.5f)) == zoneB,
		"zone.resolve.innermost");
	Check(::ZoneAtPosition(scene, glm::vec3(5.0f, 0.0f, 0.0f)) == zoneC,
		"zone.resolve.priority");
	Check(::ZoneAtPosition(scene, glm::vec3(9.0f, 0.0f, 0.0f)) == zoneA,
		"zone.resolve.outer");
	Check(::ZoneAtPosition(scene, glm::vec3(50.0f)) == UINT32_MAX,
		"zone.resolve.none");

	// Zone D: an INFINITE slab (unbounded X, bounded Y/Z) — covers any X
	// inside its Y/Z band; loses ties against bounded zones.
	NodeId zoneD = UINT32_MAX;
	{
		const int n = addNode(scene, -1, 0);
		glm::mat4 m(1.0f);
		const float inf = std::numeric_limits<float>::infinity();
		m[0] = glm::vec4(inf, 0.0f, 0.0f, 0.0f);
		m[1] = glm::vec4(0.0f, 2.0f, 0.0f, 0.0f);
		m[2] = glm::vec4(0.0f, 0.0f, 2.0f, 0.0f);
		m[3] = glm::vec4(0.0f, 5.0f, 0.0f, 1.0f);
		scene.localTransform[size_t(n)] = m;
		::SceneEnv envD{};
		SetEnv(scene, uint32_t(n), envD);
		zoneD = uint32_t(n);
		markAsChanged(scene, n);
	}
	recalculateGlobalTransforms(scene);
	Check(::ZoneCovers(scene, zoneD, glm::vec3(1e6f, 5.0f, 0.0f)),
		"zone.infinity.cover");
	Check(!::ZoneCovers(scene, zoneD, glm::vec3(0.0f, 33.0f, 0.0f)),
		"zone.infinity.boundedY");
	// Inside D's band AND inside A (both priority 0): A is bounded
	// (volume 1000) vs D unbounded -> A wins the tie (innermost).
	Check(::ZoneAtPosition(scene, glm::vec3(1.0f, 5.0f, 0.0f)) != zoneD,
		"zone.infinity.loses.ties");

	// zoneForNode: a node's world origin resolves its zone.
	{
		const int n = addNode(scene, -1, 0);
		glm::mat4 m(1.0f);
		m[3] = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f); // inside zone B
		scene.localTransform[size_t(n)] = m;
		markAsChanged(scene, n);
		recalculateGlobalTransforms(scene);
		Check(::ZoneAtPosition(scene,
				glm::vec3(scene.globalTransform[size_t(n)][3])) == zoneB,
			"zone.fornode.origin");
	}
	// World-level wrapper (the merged-scene API the hosts use).
	{
		smvk::World w;
		auto c = std::make_unique<smvk::Cluster>(nullptr);
		c->scene = scene;
		w.RegisterCluster(std::move(c));
		w.ResolveAndFixup_clusters();
		Check(w.scene.tables.envs.size() == 4, "zone.world.merged");
		const NodeId worldB = w.zoneAtPosition(
			glm::vec3(2.5f, 0.5f, 0.5f));
		Check(worldB != UINT32_MAX
			&& FindEnv(w.scene, worldB) != nullptr,
			"zone.world.resolve");
	}
}

void TestActiveEnvironmentFirstWins() {
	// Cluster carrying env blocks: root + numbered env nodes, one
	// elevation per block (exact floats — resolve copies verbatim).
	auto addEnvBlock = [](smvk::Cluster& cl, int parent, float sunEl) {
		const int n = addNode(cl.scene, parent, 0);
		SceneEnv e{};
		e.sunElevation = sunEl;
		SetEnv(cl.scene, uint32_t(n), e);
		return n;
	};
	// First scene loaded wins: A selects el 20, B selects el 30.
	// A's stored look is deliberately stale (el 99) so only the pick —
	// not the first-wins value copy — can produce el 20.
	{
		smvk::World w;
		auto a = std::make_unique<smvk::Cluster>(nullptr);
		const int aRoot = addNode(a->scene, -1, 0);
		addEnvBlock(*a, aRoot, 10.0f);
		const int aSel = addEnvBlock(*a, aRoot, 20.0f);
		a->scene.activeEnvironment = uint32_t(aSel);
		a->scene.environment.sunElevation = 99.0f;
		a->scene.envAuthored = true;
		w.RegisterCluster(std::move(a));
		auto b = std::make_unique<smvk::Cluster>(nullptr);
		const int bRoot = addNode(b->scene, -1, 0);
		const int bSel = addEnvBlock(*b, bRoot, 30.0f);
		b->scene.activeEnvironment = uint32_t(bSel);
		b->scene.envAuthored = true;
		w.RegisterCluster(std::move(b));
		w.ResolveAndFixup_clusters();
		Check(w.scene.activeEnvironment != UINT32_MAX, "actenv.kept");
		const EnvRow* it =
			FindEnv(w.scene, w.scene.activeEnvironment);
		Check(it != nullptr, "actenv.in-table");
		Check(it != nullptr
			&& it->environment.sunElevation == 20.0f, "actenv.first-wins");
		Check(w.scene.environment.sunElevation == 20.0f,
			"actenv.resolved-over-stale");
		Check(w.scene.envAuthored, "actenv.authored");
	}
	// An unselected first scene defers: A selects nothing, B's pick wins.
	{
		smvk::World w;
		auto a = std::make_unique<smvk::Cluster>(nullptr);
		const int aRoot = addNode(a->scene, -1, 0);
		addEnvBlock(*a, aRoot, 10.0f);
		a->scene.activeEnvironment = UINT32_MAX;
		w.RegisterCluster(std::move(a));
		auto b = std::make_unique<smvk::Cluster>(nullptr);
		const int bRoot = addNode(b->scene, -1, 0);
		const int bSel = addEnvBlock(*b, bRoot, 30.0f);
		b->scene.activeEnvironment = uint32_t(bSel);
		w.RegisterCluster(std::move(b));
		w.ResolveAndFixup_clusters();
		Check(w.scene.activeEnvironment != UINT32_MAX, "actenv.defer-kept");
		Check(w.scene.environment.sunElevation == 30.0f,
			"actenv.defer-resolved");
	}
	// Dangling selection (valid node, no block) clears to none and the
	// look falls back to the default block.
	{
		smvk::World w;
		auto a = std::make_unique<smvk::Cluster>(nullptr);
		const int aRoot = addNode(a->scene, -1, 0);
		addEnvBlock(*a, aRoot, 10.0f);
		const int aDangle = addNode(a->scene, aRoot, 0);
		a->scene.activeEnvironment = uint32_t(aDangle);
		a->scene.envAuthored = true;
		w.RegisterCluster(std::move(a));
		w.ResolveAndFixup_clusters();
		Check(w.scene.activeEnvironment == UINT32_MAX,
			"actenv.dangling-cleared");
		Check(w.scene.environment.sunElevation == 55.0f,
			"actenv.dangling-fallback");
	}
}

// Epoch 6: the resolve carries the previously cluster-scene-only
// tables (giVolume/cubemap/export) + metadata uuids into the world
// scene, with overlapping cluster-local node numbers remapped apart.
void TestResolveCarriesInlineTables()
{
	smvk::World w;
	auto c1 = std::make_unique<smvk::Cluster>(nullptr);
	c1->SetOfflineMode(true);
	auto c2 = std::make_unique<smvk::Cluster>(nullptr);
	c2->SetOfflineMode(true);
	// OVERLAPPING local ids: node 1 in both clusters.
	for (smvk::Cluster* c : { c1.get(), c2.get() }) {
		addNode(c->scene, -1, 0);
		addNode(c->scene, 0, 1);
	}
	::SceneGiVolume gv{};
	gv.priority = 7;
	SetGiVolume(c1->scene, 1, gv);
	::SceneCubemap cb{};
	cb.radius = 9.5f;
	SetCubemap(c2->scene, 1, cb);
	::SceneExportSettings es{};
	es.lodError = 0.25f;
	SetExport(c2->scene, 1, es);
	SetNodeUuidRaw(c1->scene, 1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	setNodeName(c1->scene, 1, "GIV_a");
	w.RegisterCluster(std::move(c1));
	w.RegisterCluster(std::move(c2));
	w.ResolveAndFixup_clusters();
	const ::Scene& s = w.scene;
	size_t gi = 0, cbm = 0, ex = 0;
	for (const GiVolumeRow& r : s.tables.giVolumes) {
		++gi;
		Check(r.volume.priority == 7, "resolve.givolume-carried");
	}
	for (const CubemapRow& r : s.tables.cubemaps) {
		++cbm;
		Check(r.cubemap.radius == 9.5f, "resolve.cubemap-carried");
	}
	for (const ExportRow& r : s.tables.exports) {
		++ex;
		Check(r.settings.lodError == 0.25f, "resolve.export-carried");
	}
	Check(gi == 1 && cbm == 1 && ex == 1, "resolve.inline-table-counts");
	// Metadata never OVER-allocates past the slot count (the pre-6b
	// resolve's base-offset append double-allocated a phantom second
	// region — 7 entries for 5 nodes). It may legitimately LAG the
	// node count: the lazy contract (addNode doesn't grow it;
	// entries beyond read as unnamed).
	Check(s.metadata.size() <= s.hierarchy.size(),
		"resolve.metadata-bounded");
	// Overlap remapped apart: both rows live at distinct world nodes.
	const GiVolumeRow* g1 = nullptr;
	for (const GiVolumeRow& r : s.tables.giVolumes)
		if (r.volume.priority == 7) g1 = &r;
	const CubemapRow* k2 = nullptr;
	for (const CubemapRow& r : s.tables.cubemaps)
		if (r.cubemap.radius == 9.5f) k2 = &r;
	Check(g1 != nullptr, "resolve.givolume-row-found");
	Check(k2 != nullptr, "resolve.cubemap-row-found");
	// Identity AT THE ROW'S OWN NODE (the whole-array uuid search this
	// test used before masked a real indexing bug: the pre-6b resolve
	// wrote names/uuids past the appended nodes, so the uuid existed
	// SOMEWHERE and the weak check passed while getNodeName/GetNodeUuid
	// answered empty at the actual node). Name AND uuid must land on
	// the exact node that carries the row.
	if (g1 && k2) {
		Check(g1->node != k2->node, "resolve.overlap-remapped-apart");
		Check(GetNodeUuid(s, g1->node)
			== "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			"resolve.uuid-at-row-node");
		Check(getNodeName(s, int(g1->node)) == "GIV_a",
			"resolve.name-at-row-node");
	}
	// Rebuild the derived index from scratch: identical answers.
	RebuildNodeRows(w.scene);
	if (g1)
		Check(FindGiVolume(w.scene, g1->node) != nullptr,
			"resolve.rebuild-stale-free");
}

// Epoch 6b: the resolve's SLOT-ASSOCIATION map — a cluster scene with
// a retired slot (a hole) resolves correctly: live nodes land with
// intact links, the hole never enters the world scene, and node-id
// refs (defaultCameraNode) ride the map instead of +offset arithmetic.
void TestResolveHoledCluster()
{
	smvk::World w;
	auto c = std::make_unique<smvk::Cluster>(nullptr);
	c->SetOfflineMode(true);
	::Scene& cs = c->scene;
	const int root = addNode(cs, -1, 0);
	const int a = addNode(cs, root, 1);
	const int b = addNode(cs, root, 1);
	SetNodeUuidRaw(cs, uint32_t(b), "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	// The cook-time static-merge shape: retire a MIDDLE subtree.
	deleteSceneNodes(cs, { (uint32_t)a });
	Check(LiveNodeCount(cs) == 2 && cs.hierarchy.size() == 3,
		"holedresolve.cluster-hole");
	cs.defaultCameraNode = uint32_t(b);
	w.RegisterCluster(std::move(c));
	w.ResolveAndFixup_clusters();
	const ::Scene& s = w.scene;
	// The world scene is freshly wiped — always dense here.
	Check(s.hierarchy.size() == 3 && LiveNodeCount(s) == 3,
		"holedresolve.world-dense");
	// Layout: 0 = world root, 1 = cluster root, 2 = b. The hole never
	// entered; b links under the cluster root with no sibling ghosts.
	Check(s.hierarchy[0].firstChild == 1 && s.hierarchy[1].parent == 0,
		"holedresolve.cluster-root-reparented");
	Check(s.hierarchy[1].firstChild == 2 && s.hierarchy[2].parent == 1
		&& s.hierarchy[2].nextSibling == -1 && s.hierarchy[1].nextSibling == -1,
		"holedresolve.links-skipped-hole");
	Check(s.metadata.size() > 2
		&& s.metadata[2].uuid == "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
		"holedresolve.uuid-through-map");
	Check(s.defaultCameraNode == 2,
		"holedresolve.default-camera-through-map");
}

} // namespace

int main() {
	TestUnloadAndDynamicCluster();
	TestDecalEntities();
	TestAudioSourceEntities();
	TestMeshEntitiesAndAnimators();
	TestBakePayloadChannel();
	TestEnvZones();
	TestActiveEnvironmentFirstWins();
	TestResolveCarriesInlineTables();
	TestResolveHoledCluster();
	std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << " ("
		<< g_checks << " checks)\n";
	return g_failures == 0 ? 0 : 1;
}
