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
// world — cluster-scene lightForNode stores the CLUSTER-LOCAL pool index,
// the resolve remap rewrites it to the world lightLocations index).
smvk::ClusterLightHandle AddLight(smvk::Cluster& cl, const char* name,
	float intensity) {
	::SceneLight def{};
	def.type = ::SceneLight::Type::Point;
	def.intensity = intensity;
	def.name = name;
	auto handle = cl.CreateLight(def);
	const int node = addNode(cl.scene, -1, 0);
	cl.scene.lightForNode[uint32_t(node)] = handle.index();
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
	// cluster: every lightForNode entry resolves into the surviving table.
	bool linksOk = true;
	for (const auto& [node, idx] : w.scene.lightForNode)
		linksOk = linksOk && idx < w.lightLocations.size();
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
		a->scene.decalForNode[uint32_t(node)] = h.index();
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
	Check(w.scene.decalForNode.count(node) == 1, "decal.create.link");
	Check(w.decalLocations[w.scene.decalForNode[node]].cluster != nullptr
		&& w.decalLocations[w.scene.decalForNode[node]]
			.cluster->isDynamic, "decal.create.dynamic");
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
	for (const auto& [n, idx] : w.scene.decalForNode)
		foldedLink = foldedLink || idx < w.decalLocations.size();
	Check(foldedLink, "decal.unload.folded.link");
	// The folded node carries the same placement frame (cluster-scene
	// original, not the merged-scene mirror).
	for (const auto& [n, idx] : w.scene.decalForNode) {
		const glm::mat4& fg = w.scene.globalTransform[n];
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
		scene.envForNode[uint32_t(n)] = envA;
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
		scene.envForNode[uint32_t(n)] = envB;
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
		scene.envForNode[uint32_t(n)] = envC;
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
		scene.envForNode[uint32_t(n)] = envD;
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
		Check(w.scene.envForNode.size() == 4, "zone.world.merged");
		const NodeId worldB = w.zoneAtPosition(
			glm::vec3(2.5f, 0.5f, 0.5f));
		Check(worldB != UINT32_MAX
			&& w.scene.envForNode.count(worldB) == 1,
			"zone.world.resolve");
	}
}

} // namespace

int main() {
	TestUnloadAndDynamicCluster();
	TestDecalEntities();
	TestBakePayloadChannel();
	TestEnvZones();
	std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << " ("
		<< g_checks << " checks)\n";
	return g_failures == 0 ? 0 : 1;
}
