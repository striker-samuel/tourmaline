// test_tour: headless unit tests for the .tour ASCII scene overlay
// (parse/serialize round-trip, reparent, cook-cluster replay). No GPU
// required. Run via CTest.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <Engine/App/cluster.h>
#include <Engine/Asset/tour.h>
#include <Engine/Scene/scene.h>

#include <gltf_import/gltf.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

std::string TempTour() {
	return (std::filesystem::temp_directory_path() / "vulkantoy_test.tour").string();
}

void TestRoundTrip() {
	smvk::tour::Doc doc;
	doc.source = "content/models/Bistro/bistro.gltf";
	doc.fingerprint = "abc:123";
	smvk::tour::Command c{};
	c.verb = smvk::tour::Verb::Rename;
	c.node = 45;
	c.text = "Shop \"Wall\"";
	c.name = "OldName";
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetTranslate;
	c.node = 7;
	c.vec = glm::vec3(1.25f, -3.5f, 0.0f);
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetRotate;
	c.node = 7;
	c.vec = glm::vec3(0.0f, 90.0f, 0.0f);
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetScale;
	c.node = 7;
	c.vec = glm::vec3(1.0f, 2.0f, 1.0f);
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::AssignMaterial;
	c.node = 9;
	c.material = 12;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetMatAttr;
	c.material = 3;
	c.text = "baseColor";
	c.vec4 = glm::vec4(1.0f, 0.5f, 0.25f, 1.0f);
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetMatAttr;
	c.material = 3;
	c.text = "metallic";
	c.number = 0.9;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetMatAttr;
	c.material = 3;
	c.text = "alphaMode";
	c.number = 1.0;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetNoLightmap;
	c.node = 45;
	c.flag = true;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetVisibility;
	c.node = 46;
	c.flag = true;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::Parent;
	c.node = 45;
	c.node2 = 12;
	doc.commands.push_back(c);
	// Wide Clarisse-style overrides (ordered, later wins, locals win over all).
	smvk::tour::OverrideRule r{};
	r.attr = smvk::tour::OverrideAttr::Visibility;
	r.pattern = "Building_.*";
	r.flag = true;
	r.enabled = true;
	doc.overrides.push_back(r);
	r = smvk::tour::OverrideRule{};
	r.attr = smvk::tour::OverrideAttr::NoLightmap;
	r.pattern = "Tree.*";
	r.flag = true;
	r.enabled = false;
	doc.overrides.push_back(r);
	doc.hasCook = true;
	doc.cook.outPak = "content/paks/x.pak";
	doc.cook.lightmap = true;
	doc.cook.lmMetersPerTexel = 0.125f;
	doc.cook.lmExcludeKw = "tree,glass";

	std::string err;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "tour save");
	if (!err.empty())
		std::cout << "  save err: " << err << "\n";

	smvk::tour::Doc back;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns), "tour load");
	Check(back.source == doc.source, "tour source");
	Check(back.fingerprint == doc.fingerprint, "tour fingerprint");
	Check(back.commands.size() == doc.commands.size(), "tour command count");
	Check(back.hasCook && back.cook.outPak == doc.cook.outPak, "tour cook outPak");
	Check(back.cook.lightmap && back.cook.lmMetersPerTexel == 0.125f,
		"tour cook bake");
	Check(back.cook.lmExcludeKw == "tree,glass", "tour cook exclude");
	if (!back.commands.empty()) {
		Check(back.commands[0].verb == smvk::tour::Verb::Rename &&
			back.commands[0].text == "Shop \"Wall\"", "tour rename quoting");
		Check(back.commands[1].verb == smvk::tour::Verb::SetTranslate &&
			back.commands[1].vec.x == 1.25f, "tour translate");
		Check(back.commands[5].verb == smvk::tour::Verb::SetMatAttr &&
			back.commands[5].vec4.y == 0.5f, "tour baseColor");
		Check(back.commands[6].number == 0.9, "tour metallic");
		Check(back.commands[7].number == 1.0, "tour alphaMode mask");
		Check(back.commands[8].flag == true, "tour noLightmap");
		Check(back.commands[9].verb == smvk::tour::Verb::SetVisibility &&
			back.commands[9].flag == true, "tour visibility");
		Check(back.commands[10].node2 == 12, "tour parent");
	}
	Check(back.overrides.size() == 2, "tour override count");
	if (back.overrides.size() == 2) {
		Check(back.overrides[0].attr ==
			smvk::tour::OverrideAttr::Visibility &&
			back.overrides[0].pattern == "Building_.*" &&
			back.overrides[0].flag == true &&
			back.overrides[0].enabled, "tour override visibility");
		Check(back.overrides[1].attr ==
			smvk::tour::OverrideAttr::NoLightmap &&
			!back.overrides[1].enabled, "tour override disabled");
		Check(smvk::tour::OverridePatternMatches("Building_.*",
			"Building_A"), "tour override match");
		Check(!smvk::tour::OverridePatternMatches("Building_.*",
			"Tree_01"), "tour override non-match");
		Check(!smvk::tour::OverridePatternMatches("([bad",
			"anything"), "tour override bad regex");
	}
	std::error_code ec;
	std::filesystem::remove(TempTour(), ec);
}

void TestUnknownVerbSkipped() {
	const std::string tp = TempTour();
	{
		std::ofstream f(tp, std::ios::binary | std::ios::trunc);
		f << "// test\nsource -p \"x.gltf\";\n";
		f << "frobnicate -n 1 -v 2;\n";       // unknown: skip
		f << "rename -n 2 PlainName;\n";      // unquoted positional + glued ;
		f << "setNoLightmap -n 3 -v 1;\n";    // known: keep
		f << "setAttr -n 4 -at glow -t 1 2 3;\n"; // unknown attr: skip
	}
	smvk::tour::Doc doc;
	std::string err;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(tp, doc, err, &warns), "tour tolerant load");
	Check(doc.commands.size() == 2, "tour skips unknowns");
	Check(warns.size() == 2, "tour warns twice");
	Check(doc.commands[0].verb == smvk::tour::Verb::Rename &&
		doc.commands[0].text == "PlainName", "tour plain rename");
	std::error_code ec;
	std::filesystem::remove(tp, ec);
}

void TestReparent() {
	Scene s{};
	const int root = addNode(s, -1, 0);
	const int a = addNode(s, root, 1);
	const int b = addNode(s, root, 1);
	const int kid = addNode(s, a, 2);
	smvk::tour::TourReparent(s, uint32_t(kid), uint32_t(b));
	Check(s.hierarchy[size_t(kid)].parent == b, "reparent link");
	Check(s.hierarchy[size_t(kid)].level == 2, "reparent level");
	Check(s.hierarchy[size_t(a)].firstChild == -1, "reparent unlink");
	// Cycle guard: parenting an ancestor under its descendant is ignored.
	smvk::tour::TourReparent(s, uint32_t(root), uint32_t(kid));
	Check(s.hierarchy[size_t(root)].parent == -1, "reparent cycle guard");
	// Root guard.
	smvk::tour::TourReparent(s, 0, uint32_t(b));
	Check(s.hierarchy[0].parent == -1, "reparent root guard");
	(void)a;
}

void TestReplay() {
	// GPU-free cook cluster with a hand-built scene (no import needed).
	// NOTE: heap-allocated — Cluster embeds multi-MB pool arrays and must
	// never live on the (1MB) thread stack (see PakCooker).
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	const int root = addNode(cluster->scene, -1, 0);
	const int g0 = addNode(cluster->scene, root, 1);
	const int g1 = addNode(cluster->scene, root, 1);
	setNodeName(cluster->scene, g0, "Wall");
	setNodeName(cluster->scene, g1, "Floor");
	smvk::MaterialCreateInfo ci{};
	ci.baseColorFactor = glm::vec4(1.0f);
	smvk::ClusterMaterialHandle m0 = cluster->CreateMaterial(ci);
	smvk::ClusterMaterialHandle m1 = cluster->CreateMaterial(ci);
	cluster->scene.materialForNode[uint32_t(g0)] = m0.index();
	cluster->scene.materialForNode[uint32_t(g1)] = m1.index();
	RebuildStaticCache(cluster->scene);

	smvk::tour::Doc doc;
	smvk::tour::Command c{};
	c.verb = smvk::tour::Verb::Rename;
	c.node = uint32_t(g0);
	c.text = "Shop_Wall";
	c.name = "Wall";
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::AssignMaterial;
	c.node = uint32_t(g0);
	c.material = m1.index();
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetMatAttr;
	c.material = m1.index();
	c.text = "metallic";
	c.number = 0.75;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetNoLightmap;
	c.node = uint32_t(g1);
	c.flag = true;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetTranslate;
	c.node = uint32_t(g1);
	c.vec = glm::vec3(1.0f, 2.0f, 3.0f);
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetVisibility;
	c.node = uint32_t(g0);
	c.flag = true;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::Parent;
	c.node = uint32_t(g0);
	c.node2 = uint32_t(g1);
	doc.commands.push_back(c);
	// Wide overrides: hide everything (g0 has a LOCAL visibility command,
	// so the rule must skip it; g1 hides via the rule). A second rule
	// later in the list shows Floor again (later wins).
	smvk::tour::OverrideRule r{};
	r.attr = smvk::tour::OverrideAttr::Visibility;
	r.pattern = ".*";
	r.flag = true;
	doc.overrides.push_back(r);
	r = smvk::tour::OverrideRule{};
	r.attr = smvk::tour::OverrideAttr::Visibility;
	r.pattern = "Flo.*";
	r.flag = false;
	doc.overrides.push_back(r);
	r = smvk::tour::OverrideRule{};
	r.attr = smvk::tour::OverrideAttr::Material;
	r.pattern = ".*"; // matches both: local wins on g0, rule applies to g1
	r.material = m0.index();
	doc.overrides.push_back(r);

	std::vector<std::string> warns;
	std::string err;
	Check(smvk::tour::ApplyTourToCluster(*cluster, doc, &warns, &err),
		"tour replay ok");
	Check(getNodeName(cluster->scene, g0) == "Shop_Wall", "replay rename");
	Check(cluster->scene.materialForNode[uint32_t(g0)] == m1.index(),
		"replay assign");
	const smvk::Material* mat = cluster->materials.get(m1);
	Check(mat && mat->values.params.y == 0.75f, "replay mattr");
	Check(cluster->scene.noLightmap.size() == cluster->scene.hierarchy.size() &&
		cluster->scene.noLightmap[size_t(g1)] == 1, "replay noLM");
	Check(cluster->scene.localTransform[size_t(g1)][3].x == 1.0f, "replay TRS");
	Check(cluster->scene.hierarchy[size_t(g0)].parent == g1, "replay parent");
	// Visibility: local hides g0; the wide ".*" rule skipped it (local
	// wins) and hit g1 instead; the later "Flo.*" rule re-showed g1.
	Check(cluster->scene.hidden.size() == cluster->scene.hierarchy.size() &&
		cluster->scene.hidden[size_t(g0)] == 1, "replay visibility local");
	Check(cluster->scene.hidden[size_t(g1)] == 0, "replay visibility later-wins");
	Check(!IsVisibleNode(cluster->scene, g0), "replay hidden effective");
	// Material override ".*": g0 carries a LOCAL AssignMaterial (m1) — the
	// wide rule must not clobber it (local wins); g1 has no local, so the
	// rule assigns m0 there.
	Check(cluster->scene.materialForNode[uint32_t(g0)] == m1.index(),
		"replay override local-wins");
	Check(cluster->scene.materialForNode[uint32_t(g1)] == m0.index(),
		"replay override applies");
	Check(smvk::tour::OverrideAttrFromString("visibility", r.attr) &&
		r.attr == smvk::tour::OverrideAttr::Visibility, "override attr parse");

	// Out-of-range ids fail loudly.
	smvk::tour::Doc bad;
	smvk::tour::Command bc{};
	bc.verb = smvk::tour::Verb::Rename;
	bc.node = 9999;
	bc.text = "x";
	bad.commands.push_back(bc);
	Check(!smvk::tour::ApplyTourToCluster(*cluster, bad, nullptr, &err),
		"replay rejects bad node");
}

// Clarisse-style nested scene references: round-trip in the .tour,
// recursive inlining (wheel -> gear chain) and the cycle guard.
void TestReferences() {
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "vulkantoy_refs";
	std::error_code ec;
	fs::create_directories(dir, ec);
	auto writeGltf = [&](const char* file, const char* nodeName) {
		const fs::path p = dir / file;
		std::ofstream f(p, std::ios::binary | std::ios::trunc);
		f << "{\"asset\":{\"version\":\"2.0\"},"
			"\"scenes\":[{\"nodes\":[0]}],"
			"\"nodes\":[{\"name\":\"" << nodeName << "\"}]}";
		return p;
	};
	writeGltf("gear.gltf", "Gear");
	writeGltf("wheel.gltf", "Wheel");
	// wheel's OWN tour references gear -> shot(wheel) -> wheel(gear).
	{
		std::ofstream f(dir / "wheel.tour", std::ios::binary | std::ios::trunc);
		f << "source -p \"wheel.gltf\";\n"
			"reference -n 0 -source \"gear.gltf\" -name \"gear\";\n";
	}
	// Round-trip in the master tour file.
	smvk::tour::Doc doc;
	smvk::tour::ReferenceCmd r{};
	r.parent = 0;
	r.source = "wheel.gltf";
	r.name = "wheel";
	r.translate = glm::vec3(1.0f, 2.0f, 3.0f);
	r.hasTranslate = true;
	doc.references.push_back(r);
	std::string err;
	const std::string masterTour = (dir / "master.tour").string();
	Check(smvk::tour::SaveTour(masterTour, doc, err), "ref save");
	smvk::tour::Doc back;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(masterTour, back, err, &warns), "ref load");
	Check(back.references.size() == 1, "ref count");
	if (back.references.size() == 1) {
		Check(back.references[0].source == "wheel.gltf" &&
			back.references[0].name == "wheel" &&
			back.references[0].parent == 0 &&
			back.references[0].hasTranslate &&
			back.references[0].translate.y == 2.0f, "ref fields");
	}
	// Apply to a cook cluster: wheel inlined + nested gear (BFS).
	// NOTE: heap-allocated (Cluster embeds multi-MB pools).
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	addNode(cluster->scene, -1, 0); // cluster root (importer parity)
	std::vector<int> ph;
	const size_t applied = ApplySceneReferences(*cluster, back,
		dir.generic_string(), nullptr, nullptr, nullptr,
		smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0, &ph);
	Check(applied == 2, "ref nested applied (wheel + gear)");
	Check(ph.size() == 1, "ref placeholder list");
	Check(findNodeByName(cluster->scene, "REF wheel") >= 0, "ref placeholder node");
	Check(findNodeByName(cluster->scene, "Wheel") >= 0, "ref subtree imported");
	Check(findNodeByName(cluster->scene, "REF gear") >= 0, "ref nested placeholder");
	Check(findNodeByName(cluster->scene, "Gear") >= 0, "ref nested subtree");
	// Cycle guard: a scene whose tour references itself inlines once.
	writeGltf("self.gltf", "Self");
	{
		std::ofstream f(dir / "self.tour", std::ios::binary | std::ios::trunc);
		f << "reference -n 0 -source \"self.gltf\";\n";
	}
	smvk::tour::Doc selfDoc;
	smvk::tour::ReferenceCmd sr{};
	sr.parent = 0;
	sr.source = "self.gltf";
	selfDoc.references.push_back(sr);
	auto cluster2 = std::make_unique<smvk::Cluster>(nullptr);
	cluster2->SetCookMode(true);
	addNode(cluster2->scene, -1, 0);
	const size_t selfApplied = ApplySceneReferences(*cluster2, selfDoc,
		dir.generic_string(), nullptr, nullptr, nullptr,
		smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0, nullptr);
	Check(selfApplied == 1, "ref cycle guard (inlined once)");
	// Cleanup.
	fs::remove_all(dir, ec);
}

void TestTourReferences() {
	// .tour as a reference source: the authored scene imports, then its
	// tour commands replay with id offsets (rename/transform/createLight
	// + setLightAttr -ref). Disabled references stay unbound.
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "vulkantoy_tourrefs";
	std::error_code ec;
	fs::create_directories(dir, ec);
	auto writeGltf = [&](const char* file, const char* nodeName) {
		const fs::path p = dir / file;
		std::ofstream f(p, std::ios::binary | std::ios::trunc);
		f << "{\"asset\":{\"version\":\"2.0\"},"
			"\"scenes\":[{\"nodes\":[0]}],"
			"\"nodes\":[{\"name\":\"" << nodeName << "\"}]}";
		return p;
	};
	writeGltf("sub.gltf", "Box");
	writeGltf("gear.gltf", "Gear");
	// The referenced tour: rename + move its node, nest a gltf, create
	// a light and tune it via -ref.
	{
		std::ofstream f(dir / "sub.tour", std::ios::binary | std::ios::trunc);
		f << "source -p \"sub.gltf\";\n"
			"rename -n 0 \"BoxRenamed\";\n"
			"setAttr -n 0 -at translate -t 5 0 0;\n"
			"reference -n 0 -source \"gear.gltf\" -name \"gear\";\n"
			"createLight -p 0 -name \"Sub Spot\" -at spot -t 1 2 3 100 10 15 30 0 1;\n"
			"setLightAttr -ref 0 -at intensity -v 200;\n";
	}
	auto freshCluster = []() {
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetCookMode(true);
		addNode(cluster->scene, -1, 0); // cluster root (importer parity)
		return cluster;
	};
	// Master references the .tour (enabled by default).
	smvk::tour::Doc doc;
	smvk::tour::ReferenceCmd r{};
	r.parent = 0;
	r.source = "sub.tour";
	r.name = "sub";
	doc.references.push_back(r);
	std::string err;
	std::vector<std::string> warns;
	auto cluster = freshCluster();
	std::vector<int> ph;
	const size_t applied = ApplySceneReferences(*cluster, doc,
		dir.generic_string(), nullptr, nullptr, nullptr,
		smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0, &ph);
	// sub inlined + its tour replayed + nested gear inlined.
	Check(applied == 2, "tourref applied (sub + nested gear)");
	Check(ph.size() == 1, "tourref placeholder list");
	// Id layout: 0 root, 1 "REF sub", 2 BoxRenamed, 3 created light
	// (+ gear placeholder + Gear appended after).
	Check(findNodeByName(cluster->scene, "REF sub") == 1,
		"tourref placeholder");
	Check(findNodeByName(cluster->scene, "BoxRenamed") == 2,
		"tourref replayed rename with offset");
	Check(cluster->scene.hierarchy[size_t(2)].parent == 1,
		"tourref subtree parent");
	Check(cluster->scene.localTransform[size_t(2)][3].x == 5.0f,
		"tourref replayed transform with offset");
	Check(findNodeByName(cluster->scene, "Sub Spot") == 3,
		"tourref created light with offset parent");
	const uint32_t lightNode = 3;
	auto lit = cluster->scene.lightForNode.find(lightNode);
	Check(lit != cluster->scene.lightForNode.end(), "tourref light link");
	if (lit != cluster->scene.lightForNode.end()) {
		const ::SceneLight* L = cluster->lights.get(
			cluster->lights.getHandle(lit->second));
		Check(L && L->type == ::SceneLight::Type::Spot
			&& L->intensity == 200.0f
			&& (L->flags & ::SceneLight::kSceneLightFlag_CastShadow) != 0,
			"tourref -ref attr applied in replay");
	}
	Check(findNodeByName(cluster->scene, "REF gear") >= 0,
		"tourref nested placeholder");
	Check(findNodeByName(cluster->scene, "Gear") >= 0,
		"tourref nested subtree");

	// Disabled reference: nothing binds.
	smvk::tour::Doc offDoc;
	smvk::tour::ReferenceCmd off{};
	off.parent = 0;
	off.source = "sub.tour";
	off.name = "sub";
	off.enabled = false;
	offDoc.references.push_back(off);
	auto clusterOff = freshCluster();
	const size_t offApplied = ApplySceneReferences(*clusterOff, offDoc,
		dir.generic_string(), nullptr, nullptr, nullptr,
		smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0, nullptr);
	Check(offApplied == 0, "tourref disabled skips bind");
	Check(findNodeByName(clusterOff->scene, "REF sub") < 0,
		"tourref disabled has no placeholder");

	// -enabled round-trips (absent = enabled).
	smvk::tour::Doc rtDoc;
	rtDoc.references.push_back(off);
	std::string rtErr;
	Check(smvk::tour::SaveTour((dir / "rt.tour").string(), rtDoc, rtErr),
		"tourref enabled save");
	smvk::tour::Doc rtBack;
	Check(smvk::tour::LoadTour((dir / "rt.tour").string(), rtBack, rtErr),
		"tourref enabled load");
	Check(rtBack.references.size() == 1 && !rtBack.references[0].enabled,
		"tourref enabled flag roundtrip");
	// Cleanup.
	fs::remove_all(dir, ec);
}

void TestLightVerbs() {
	// Editor-authored lights: createLight/setLightAttr round-trip through
	// Editor-authored lights: createLight/setLightAttr round-trip through
	// the .tour file, then replay onto a cook cluster (create appends a
	// node + pool definition; -ref addressing hits creation order).
	smvk::tour::Doc doc;
	doc.source = "models/x.gltf";
	smvk::tour::Command c{};
	c.verb = smvk::tour::Verb::CreateLight;
	c.node = 0; // parent: cluster root
	c.text = "Key Spot";
	c.vec = glm::vec3(1.0f, 2.0f, 3.0f);
	c.vec4 = glm::vec4(2.0f, 1500.0f, 25.0f, 20.0f); // spot, I, range, inner
	c.number = 35.0; // outer
	c.flag = false;  // dynamic
	c.node2 = 1;     // shadow
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetLightAttr;
	c.node = smvk::tour::kCreateLightRefBase | 0u;
	c.text = "intensity";
	c.number = 2000.0;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetLightAttr;
	c.node = smvk::tour::kCreateLightRefBase | 0u;
	c.text = "color";
	c.vec4 = glm::vec4(1.0f, 0.5f, 0.25f, 1.0f);
	doc.commands.push_back(c);

	std::string err;
	std::vector<std::string> warns;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "light tour save");
	smvk::tour::Doc back;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns), "light tour load");
	Check(back.commands.size() == 3, "light tour command count");
	const smvk::tour::Command& rc = back.commands[0];
	Check(rc.verb == smvk::tour::Verb::CreateLight && rc.node == 0
		&& rc.text == "Key Spot" && rc.vec4.x == 2.0f
		&& rc.vec4.y == 1500.0f && rc.number == 35.0 && !rc.flag
		&& rc.node2 == 1, "light create roundtrip");
	Check(back.commands[1].verb == smvk::tour::Verb::SetLightAttr
		&& back.commands[1].node == (smvk::tour::kCreateLightRefBase | 0u)
		&& back.commands[1].text == "intensity"
		&& back.commands[1].number == 2000.0, "light attr roundtrip");

	// Replay: root + one imported node; create under root.
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	const int root = addNode(cluster->scene, -1, 0);
	setNodeName(cluster->scene, root, "Root");
	const size_t beforeNodes = cluster->scene.hierarchy.size();
	Check(smvk::tour::ApplyTourToCluster(*cluster, back, &warns, &err),
		"light replay ok");
	Check(cluster->scene.hierarchy.size() == beforeNodes + 1,
		"light replay appends node");
	const uint32_t newNode = uint32_t(beforeNodes);
	Check(cluster->scene.hierarchy[newNode].parent == root,
		"light replay parent");
	Check(getNodeName(cluster->scene, int(newNode)) == "Key Spot",
		"light replay name");
	auto lit = cluster->scene.lightForNode.find(newNode);
	Check(lit != cluster->scene.lightForNode.end(), "light replay link");
	const ::SceneLight* L = cluster->lights.get(
		cluster->lights.getHandle(lit->second));
	bool params = L && L->type == ::SceneLight::Type::Spot
		&& L->intensity == 2000.0f && L->range == 25.0f
		&& std::abs(L->innerConeAngle - glm::radians(20.0f)) < 1e-4f
		&& std::abs(L->outerConeAngle - glm::radians(35.0f)) < 1e-4f
		&& (L->flags & ::SceneLight::kSceneLightFlag_CastShadow) != 0
		&& (L->flags & ::SceneLight::kSceneLightFlag_Static) == 0
		&& L->color == glm::vec3(1.0f, 0.5f, 0.25f);
	Check(params, "light replay params (-ref intensity + color applied)");
	Check(cluster->scene.localTransform[newNode][3].x == 1.0f,
		"light replay position");

	// Out-of-range -ref fails loudly; unknown attr fails at parse.
	smvk::tour::Doc bad;
	smvk::tour::Command bc{};
	bc.verb = smvk::tour::Verb::SetLightAttr;
	bc.node = smvk::tour::kCreateLightRefBase | 5u;
	bc.text = "intensity";
	bc.number = 1.0;
	bad.commands.push_back(bc);
	Check(!smvk::tour::ApplyTourToCluster(*cluster, bad, nullptr, &err),
		"light replay rejects bad ref");
	smvk::tour::Doc parseBad;
	Check(!smvk::tour::LoadTourFromString(
		"setLightAttr -n 0 -at frobnicate -v 1;\n", parseBad, err),
		"light parser rejects bad attr");
}

void TestTerrainVerbs() {
	// createTerrain (group + tiles) + terrainPaint: file round-trip, then
	// cook replay — heights bake through the stamps into the tile meshes.
	smvk::tour::Doc doc;
	doc.source = "models/x.gltf";
	smvk::tour::Command c{};
	c.verb = smvk::tour::Verb::CreateTerrain;
	c.node = 0; // parent: cluster root
	c.text = "Terrain_1";
	c.vec = glm::vec3(10.0f, 0.0f, 20.0f);
	c.vec4 = glm::vec4(32.0f, 17.0f, 4.0f, 0.05f); // size, res, height, freq
	c.number = 7.0;                                // seed
	c.node2 = 2u | (2u << 16);                     // 2x2 tiles
	c.material = 0;                                // out-of-range -> default
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::TerrainPaint;
	c.node = smvk::tour::kCreateTerrainRefBase | 0u;
	c.text = "raise";
	c.vec4 = glm::vec4(26.0f, 36.0f, 4.0f, 0.0f); // worldX, worldZ, radius, target
	c.number = 2.0;                               // strength
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::TerrainPaint;
	c.node = smvk::tour::kCreateTerrainRefBase | 0u;
	c.text = "smooth";
	c.vec4 = glm::vec4(26.0f, 36.0f, 6.0f, 0.0f);
	c.number = 1.0;
	doc.commands.push_back(c);

	std::string err;
	std::vector<std::string> warns;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "terrain tour save");
	smvk::tour::Doc back;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns),
		"terrain tour load");
	Check(back.commands.size() == 3, "terrain tour command count");
	const smvk::tour::Command& rc = back.commands[0];
	Check(rc.verb == smvk::tour::Verb::CreateTerrain && rc.node == 0
		&& rc.text == "Terrain_1" && rc.vec4.x == 32.0f
		&& rc.vec4.y == 17.0f && rc.number == 7.0
		&& (rc.node2 & 0xFFFFu) == 2u
		&& ((rc.node2 >> 16) & 0xFFFFu) == 2u, "terrain create roundtrip");
	const smvk::tour::Command& rp = back.commands[1];
	Check(rp.verb == smvk::tour::Verb::TerrainPaint
		&& rp.node == (smvk::tour::kCreateTerrainRefBase | 0u)
		&& rp.text == "raise" && rp.number == 2.0,
		"terrain paint roundtrip");

	// Replay onto a cook cluster: group + 4 tile children + meshes from
	// the POST-PAINT heights. Material 0 is out of range -> the cook
	// mints the editor default (warned, not failed).
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	const int root = addNode(cluster->scene, -1, 0);
	setNodeName(cluster->scene, root, "Root");
	const size_t beforeNodes = cluster->scene.hierarchy.size();
	Check(smvk::tour::ApplyTourToCluster(*cluster, back, &warns, &err),
		"terrain replay ok");
	const size_t added = cluster->scene.hierarchy.size() - beforeNodes;
	Check(added == 1 + 4, "terrain replay group+4 tiles");
	const uint32_t group = uint32_t(beforeNodes);
	Check(cluster->scene.hierarchy[group].parent == root,
		"terrain replay group parent");
	Check(getNodeName(cluster->scene, int(group)) == "Terrain_1",
		"terrain replay group name");
	uint32_t tileMeshes = 0;
	for (const auto& [n, m] : cluster->scene.meshForNode) {
		(void)m;
		if (n == group)
			continue; // the group node carries no mesh
		if (cluster->scene.hierarchy[n].parent == int(group))
			++tileMeshes;
	}
	Check(tileMeshes == 4, "terrain replay 4 tile meshes");
	// Default material minted: group tiles reference a valid pool id.
	bool matOk = true;
	for (const auto& [n, mi] : cluster->scene.meshForNode) {
		auto it = cluster->scene.materialForNode.find(n);
		if (it == cluster->scene.materialForNode.end()
			|| !cluster->materials.get(cluster->materials.getHandle(it->second)))
			matOk = false;
	}
	Check(matOk, "terrain replay default material minted");
	// Post-loop mesh upload happened (heights baked through 2 stamps).
	const uint32_t nodes = uint32_t(cluster->scene.hierarchy.size());
	uint32_t withMesh = 0;
	for (uint32_t n = 0; n < nodes; ++n)
		if (cluster->scene.meshForNode.contains(n))
			++withMesh;
	Check(withMesh == 4, "terrain replay meshes uploaded post-pass");

	// Out-of-range paint ref fails loudly; bad op fails at parse.
	smvk::tour::Doc bad;
	smvk::tour::Command bc{};
	bc.verb = smvk::tour::Verb::TerrainPaint;
	bc.node = smvk::tour::kCreateTerrainRefBase | 5u;
	bc.text = "raise";
	bc.vec4 = glm::vec4(1, 1, 1, 0);
	bc.number = 1.0;
	bad.commands.push_back(bc);
	Check(!smvk::tour::ApplyTourToCluster(*cluster, bad, nullptr, &err),
		"terrain paint rejects bad ref");
	smvk::tour::Doc parseBad;
	Check(!smvk::tour::LoadTourFromString(
		"terrainPaint -ref 0 -at frobnicate -t 1 1 1 0 -v 1;\n",
		parseBad, err), "terrain paint parser rejects bad op");
}

} // namespace

int main() {
	TestRoundTrip();
	TestUnknownVerbSkipped();
	TestReparent();
	TestReplay();
	TestReferences();
	TestTourReferences();
	TestLightVerbs();
	TestTerrainVerbs();
	std::cout << g_checks - g_failures << "/" << g_checks << " passed\n";
	return g_failures == 0 ? 0 : 1;
}
