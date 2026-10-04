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
	// Meshlet + LOD detail settings ride the cook block too.
	doc.cook.lods = true;
	doc.cook.lodRatios = "0.5,0.25,0.1";
	doc.cook.lodError = 0.05f;
	doc.cook.meshlets = true;
	doc.cook.meshletMaxVerts = 100;
	doc.cook.meshletMaxTris = 200;
	doc.cook.meshletConeWeight = 0.5f;
	// The sun/sky ride the ENV BLOCKS (env.default.* editorSet keys —
	// SceneEnv, lux-native); TourCook carries no sun/sky fields anymore.
	doc.editorSettings.emplace_back("env.default.sunAzimuth", "120");
	doc.editorSettings.emplace_back("env.default.sunElevation", "42");
	doc.editorSettings.emplace_back("env.default.sunIntensity", "4242");
	doc.editorSettings.emplace_back("env.default.sunColor", "1 0.5 0.25");

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
	Check(back.cook.lods && back.cook.lodRatios == "0.5,0.25,0.1"
		&& std::abs(back.cook.lodError - 0.05f) < 1e-6f,
		"tour cook lod detail");
	Check(back.cook.meshlets && back.cook.meshletMaxVerts == 100
		&& back.cook.meshletMaxTris == 200
		&& std::abs(back.cook.meshletConeWeight - 0.5f) < 1e-6f,
		"tour cook meshlet settings");
	// The env-block sun round-trips through the editorSet section (the
	// opaque key/value pairs the editor + the cooker's env resolution
	// parse; the cook block itself no longer carries a sun). NOTE: the
	// last quoted token of a line keeps the statement ';' glued to it
	// (the tokenizer strips quotes, not the terminator) — every real
	// consumer is lenient (stof/istringstream), so trim it here only.
	bool envSunOk = false;
	{
		std::string az, el, inten, col;
		auto trimSemi = [](std::string s) {
			while (!s.empty() && (s.back() == ';'))
				s.pop_back();
			return s;
		};
		for (const auto& [k, v] : back.editorSettings) {
			if (k == "env.default.sunAzimuth") az = trimSemi(v);
			else if (k == "env.default.sunElevation") el = trimSemi(v);
			else if (k == "env.default.sunIntensity") inten = trimSemi(v);
			else if (k == "env.default.sunColor") col = trimSemi(v);
		}
		envSunOk = az == "120" && el == "42" && inten == "4242"
			&& col == "1 0.5 0.25";
	}
	Check(envSunOk, "tour env sun block (the sun of record)");
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

// Maya file-block shape: Name/Last-modified header (old source-asset
// comments gone), references first, fileInfo constants, tabulated
// create properties, -type on values — and the whole thing re-parses.
void TestTourFormat() {
	smvk::tour::Doc doc;
	doc.source = "content/models/Bistro/bistro.gltf";
	smvk::tour::ReferenceCmd r{};
	r.parent = 0;
	r.source = "props/chair.gltf";
	r.name = "chair";
	doc.references.push_back(r);
	smvk::tour::Command c{};
	c.verb = smvk::tour::Verb::SetVisibility;
	c.node = 4;
	c.flag = true;
	doc.commands.push_back(c);
	smvk::tour::Command cl{};
	cl.verb = smvk::tour::Verb::CreateLight;
	cl.node = 0;
	cl.text = "Spot_1";
	cl.vec = glm::vec3(1, 2, 3);
	cl.vec4 = glm::vec4(2.0f, 100.0f, 10.0f, 15.0f);
	cl.number = 30.0;
	cl.flag = false;
	cl.node2 = 1;
	doc.commands.push_back(cl);
	smvk::tour::Command sa{};
	sa.verb = smvk::tour::Verb::SetLightAttr;
	sa.node = smvk::tour::kCreateLightRefBase | 0u;
	sa.text = "intensity";
	sa.number = 1500.0;
	doc.commands.push_back(sa);
	smvk::tour::Command tr{};
	tr.verb = smvk::tour::Verb::SetTranslate;
	tr.node = 7;
	tr.vec = glm::vec3(1.0f, 2.0f, 3.0f);
	doc.commands.push_back(tr);
	std::string err;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "format save");
	std::ifstream in(TempTour(), std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string text = ss.str();
	in.close();
	// Header: new lines in, old comment lines out.
	Check(text.find("//Name: ") != std::string::npos, "format name header");
	Check(text.find("//Last modified: ") != std::string::npos,
		"format date header");
	Check(text.find("// source asset:") == std::string::npos,
		"format old source comment gone");
	Check(text.find("cluster-space") == std::string::npos,
		"format old cluster comment gone");
	// Block order: references, then fileInfo, then commands.
	const size_t refPos = text.find("reference -n 0");
	const size_t fiPos = text.find("fileInfo");
	const size_t createPos = text.find("createLight");
	Check(refPos != std::string::npos && fiPos != std::string::npos
		&& createPos != std::string::npos && refPos < fiPos
		&& fiPos < createPos, "format block order");
	Check(text.find("fileInfo -k \"application\"") != std::string::npos,
		"format fileinfo application");
	// Tabulated -ref property under its create + -type on values.
	Check(text.find("\tsetLightAttr -ref 0 -at intensity")
		!= std::string::npos, "format tabulated ref");
	Check(text.find("-type \"double3\"") != std::string::npos,
		"format type double3");
	Check(text.find("-type \"bool\"") != std::string::npos,
		"format type bool");
	// The shaped file re-parses identically.
	smvk::tour::Doc back;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns),
		"format reload");
	Check(back.references.size() == 1
		&& back.references[0].name == "chair", "format ref reload");
	Check(back.fileInfo.size() >= 3, "format fileinfo reload");
	Check(back.commands.size() == 4, "format command count");
	if (back.commands.size() == 4) {
		Check(back.commands[0].verb == smvk::tour::Verb::SetVisibility
			&& back.commands[0].flag, "format visibility reload");
		Check(back.commands[1].verb == smvk::tour::Verb::CreateLight
			&& back.commands[1].text == "Spot_1",
			"format create reload");
		Check(back.commands[2].verb == smvk::tour::Verb::SetLightAttr
			&& (back.commands[2].node
				& smvk::tour::kCreateLightRefBase) != 0
			&& back.commands[2].number == 1500.0,
			"format tabulated ref reload");
		Check(back.commands[3].verb == smvk::tour::Verb::SetTranslate,
			"format translate reload");
	}
	// Legacy lines without -type still load.
	{
		std::ofstream f(TempTour(), std::ios::binary | std::ios::trunc);
		f << "source -p \"x.gltf\";\n";
		f << "setAttr -n 4 -at translate -t 1 2 3;\n";
		f << "setVisibility -n 5 -v 1;\n";
		f << "setLightAttr -ref 0 -at intensity -v 42;\n";
	}
	smvk::tour::Doc legacy;
	Check(smvk::tour::LoadTour(TempTour(), legacy, err, &warns),
		"format legacy loads");
	Check(legacy.commands.size() == 3, "format legacy count");
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
	std::vector<std::string> resolved;
	const size_t applied = ApplySceneReferences(*cluster, back,
		dir.generic_string(), nullptr, nullptr, nullptr,
		smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0, &ph,
		nullptr, &resolved);
	Check(applied == 2, "ref nested applied (wheel + gear)");
	Check(ph.size() == 1, "ref placeholder list");
	Check(findNodeByName(cluster->scene, "REF wheel") >= 0, "ref placeholder node");
	// Reference namespaces (Maya): the subtree takes '<ref>:' — root
	// namespace is the master tour (its nodes carry no prefix).
	Check(findNodeByName(cluster->scene, "wheel:Wheel") >= 0,
		"ref subtree namespaced");
	Check(findNodeByName(cluster->scene, "REF gear") >= 0, "ref nested placeholder");
	Check(findNodeByName(cluster->scene, "wheel:gear:Gear") >= 0,
		"ref nested namespaced chain");
	// Resolved sources: one canonical path per doc reference, in order
	// (the raw string is what the file says; this is what bound).
	Check(resolved.size() == back.references.size(),
		"ref resolved count matches");
	if (resolved.size() == back.references.size() && !resolved.empty()) {
		Check(!resolved[0].empty()
			&& resolved[0].find("wheel.gltf") != std::string::npos,
			"ref resolved canonical path");
	}
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

// ROOT references: the scene's own glTF rides as reference #0 in the
// definition doc — inlined under the scene root with NO placeholder and
// NO namespace, so cluster ids/names match a direct import exactly
// (editor + cook build identical worlds). Round-trips as
// `reference -root -source "..."`.
void TestRootReference() {
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "vulkantoy_rootref";
	std::error_code ec;
	fs::create_directories(dir, ec);
	const fs::path gltf = dir / "scene.gltf";
	{
		std::ofstream f(gltf, std::ios::binary | std::ios::trunc);
		f << "{\"asset\":{\"version\":\"2.0\"},"
			"\"scenes\":[{\"nodes\":[0,1]}],"
			"\"nodes\":[{\"name\":\"Alpha\"},{\"name\":\"Beta\"}]}";
	}
	std::string err;
	// Round-trip: writer emits `reference -root`, parser reads it back.
	smvk::tour::Doc doc;
	smvk::tour::ReferenceCmd root{};
	root.root = true;
	root.source = "scene.gltf";
	root.name = "scene";
	doc.references.push_back(root);
	smvk::tour::ReferenceCmd other{};
	other.parent = 0;
	other.source = "extra.gltf";
	other.name = "extra";
	doc.references.push_back(other);
	const std::string tourPath = (dir / "def.tour").string();
	Check(smvk::tour::SaveTour(tourPath, doc, err), "rootref save");
	{
		std::ifstream f(tourPath, std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		const std::string text = ss.str();
		Check(text.find("reference -root -n 0 -source \"scene.gltf\"")
			!= std::string::npos, "rootref -root line");
	}
	smvk::tour::Doc back;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(tourPath, back, err, &warns),
		"rootref load");
	Check(back.references.size() == 2, "rootref count");
	if (back.references.size() == 2) {
		Check(back.references[0].root && back.references[0].source == "scene.gltf",
			"rootref roundtrip");
		Check(!back.references[1].root, "rootref second stays normal");
	}
	// Bind parity A: definition build (empty root + root reference).
	auto defCluster = std::make_unique<smvk::Cluster>(nullptr);
	defCluster->SetCookMode(true);
	addNode(defCluster->scene, -1, 0);
	std::vector<int> defPh;
	std::vector<std::string> defResolved;
	smvk::tour::Doc defDoc; // root ref only (extra.gltf does not exist)
	defDoc.references.push_back(root);
	const size_t applied = ApplySceneReferences(*defCluster, defDoc,
		dir.generic_string(), nullptr, nullptr, nullptr,
		smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0, &defPh,
		nullptr, &defResolved);
	Check(applied == 1, "rootref applied");
	// No placeholder: the sentinel entry is -1 (root refs anchor at
	// the scene root).
	Check(defPh.size() == 1 && defPh[0] == -1, "rootref placeholder sentinel");
	// Bind parity B: direct import of the same glTF.
	tinygltf::Model model;
	Check(LoadModelFromAsciiFile(model, gltf.string().c_str()),
		"rootref parse for direct import");
	auto dirCluster = std::make_unique<smvk::Cluster>(nullptr);
	dirCluster->SetCookMode(true);
	addNode(dirCluster->scene, -1, 0);
	{
		GltfToSceneImporter importer(*dirCluster, model, gltf);
		importer.setTextureSource(std::make_shared<HostTextureSource>(dir));
		// Direct-import roots: default scene's nodes, first-scene
		// fallback (the same traversal GltfToCluster uses).
		std::vector<int> gltfRoots;
		if (model.defaultScene >= 0
			&& model.defaultScene < (int)model.scenes.size())
			gltfRoots = model.scenes[size_t(model.defaultScene)].nodes;
		else if (!model.scenes.empty())
			gltfRoots = model.scenes.front().nodes;
		for (int gltfNode : gltfRoots)
			importer.import(gltfNode);
	}
	// IDENTICAL builds: same node count, same names at the same ids
	// (no placeholder node, no namespace prefix).
	Check(defCluster->scene.hierarchy.size()
			== dirCluster->scene.hierarchy.size(),
		"rootref parity node count");
	bool namesMatch = true;
	for (size_t n = 0;
		n < defCluster->scene.hierarchy.size()
			&& n < dirCluster->scene.hierarchy.size(); ++n)
		namesMatch = namesMatch && getNodeName(defCluster->scene, int(n))
			== getNodeName(dirCluster->scene, int(n));
	Check(namesMatch, "rootref parity names/ids (no placeholder, no ns)");
	Check(findNodeByName(defCluster->scene, "Alpha") == 1,
		"rootref first glTF root at id 1");
	Check(findNodeByName(defCluster->scene, "REF scene") < 0,
		"rootref makes no placeholder");
	// Root + normal reference together: the normal one keeps its
	// placeholder and namespace; the root one stays bare.
	{
		std::ofstream f(dir / "extra.gltf", std::ios::binary | std::ios::trunc);
		f << "{\"asset\":{\"version\":\"2.0\"},"
			"\"scenes\":[{\"nodes\":[0]}],"
			"\"nodes\":[{\"name\":\"Extra\"}]}";
	}
	{
		auto both = std::make_unique<smvk::Cluster>(nullptr);
		both->SetCookMode(true);
		addNode(both->scene, -1, 0);
		std::vector<int> ph2;
		const size_t bothApplied = ApplySceneReferences(*both, back,
			dir.generic_string(), nullptr, nullptr, nullptr,
			smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0, &ph2,
			nullptr, nullptr);
		Check(bothApplied == 2, "rootref mixed applied");
		Check(ph2.size() == 2 && ph2[0] == -1 && ph2[1] >= 0,
			"rootref mixed placeholders");
		Check(findNodeByName(both->scene, "extra:Extra") >= 0,
			"rootref normal ref still namespaced");
	}
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
	Check(findNodeByName(cluster->scene, "sub:BoxRenamed") == 2,
		"tourref replayed rename with offset");
	Check(cluster->scene.hierarchy[size_t(2)].parent == 1,
		"tourref subtree parent");
	Check(cluster->scene.localTransform[size_t(2)][3].x == 5.0f,
		"tourref replayed transform with offset");
	Check(findNodeByName(cluster->scene, "sub:Sub Spot") == 3,
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
	Check(findNodeByName(cluster->scene, "sub:gear:Gear") >= 0,
		"tourref nested namespaced chain");

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
	// Keep-in-scene contract: disabled references STILL inline (every
	// cluster id stays stable across enable/disable toggles, so edits
	// on referenced nodes can never be orphaned). The disabled-HIDE is
	// EDITOR-side on the resolved world: the runtime draw-list build
	// excludes hidden nodes at resolve time, so an import-side hide
	// would permanently drop the reference's meshes.
	Check(offApplied == 2, "tourref disabled binds (kept in scene)");
	const int offPh = findNodeByName(clusterOff->scene, "REF sub");
	Check(offPh >= 0, "tourref disabled keeps placeholder");
	Check(clusterOff->scene.hidden.size()
			!= clusterOff->scene.hierarchy.size()
		|| clusterOff->scene.hidden[size_t(offPh)] == 0,
		"tourref disabled NOT hidden at import (draw list intact)");
	Check(findNodeByName(clusterOff->scene, "sub:BoxRenamed") >= 0,
		"tourref disabled keeps namespaced nodes");

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

// Unified createNode form: new files write `createNode -at <type>`;
// DAG types lower to the legacy verbs at parse (single replay path);
// system types (material/file/image) parse, validate, and skip
// at replay; legacy createLight/createTerrain/createTransform lines
// load unchanged.
void TestCvolVerbs() {
	// Authored convex volumes: createCvolume/setCvolAttr round-trip
	// through the .tour file, then replay onto a cook cluster (create
	// appends a node + node-local def; -ref addressing hits creation
	// order; delete drops the def link).
	smvk::tour::Doc doc;
	doc.source = "models/x.gltf";
	smvk::tour::Command c{};
	c.verb = smvk::tour::Verb::CreateCVolume;
	c.node = 0; // parent: cluster root
	c.text = "CVol_1";
	c.vec = glm::vec3(1.0f, 2.0f, 3.0f);
	c.svec = glm::vec3(8.0f, 4.0f, 6.0f);
	c.vec4 = glm::vec4(2.0f, 0.5f, 0.1f, 0.2f); // priority, spacing, in, out
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetCvolAttr;
	c.node = smvk::tour::kCreateCvolRefBase | 0u;
	c.text = "spacing";
	c.number = 0.25;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetCvolAttr;
	c.node = smvk::tour::kCreateCvolRefBase | 0u;
	c.text = "size";
	c.vec4 = glm::vec4(10.0f, 10.0f, 10.0f, 1.0f);
	doc.commands.push_back(c);

	std::string err;
	std::vector<std::string> warns;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "cvol tour save");
	std::ifstream in(TempTour(), std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string text = ss.str();
	in.close();
	Check(text.find("createCvolume -p 0 -name \"CVol_1\"") != std::string::npos,
		"cvol legacy create line");
	Check(text.find("setCvolAttr -ref 0 -at spacing -v 0.25") != std::string::npos,
		"cvol tabulated attr line");
	smvk::tour::Doc back;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns), "cvol tour load");
	Check(back.commands.size() == 3, "cvol tour command count");
	const smvk::tour::Command& rc = back.commands[0];
	Check(rc.verb == smvk::tour::Verb::CreateCVolume && rc.node == 0
		&& rc.text == "CVol_1" && rc.vec4.x == 2.0f
		&& rc.vec4.y == 0.5f && rc.svec.x == 8.0f,
		"cvol create roundtrip");
	Check(back.commands[1].verb == smvk::tour::Verb::SetCvolAttr
		&& back.commands[1].node == (smvk::tour::kCreateCvolRefBase | 0u)
		&& back.commands[1].text == "spacing"
		&& back.commands[1].number == 0.25, "cvol attr roundtrip");
	Check(!smvk::tour::Describe(back.commands[0]).empty()
		&& !smvk::tour::Describe(back.commands[1]).empty(),
		"cvol describe");

	// Replay: root + one imported node; create under root.
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	const int root = addNode(cluster->scene, -1, 0);
	setNodeName(cluster->scene, root, "Root");
	const size_t beforeNodes = cluster->scene.hierarchy.size();
	Check(smvk::tour::ApplyTourToCluster(*cluster, back, &warns, &err),
		"cvol replay ok");
	Check(cluster->scene.hierarchy.size() == beforeNodes + 1,
		"cvol replay appends node");
	const uint32_t newNode = uint32_t(beforeNodes);
	Check(cluster->scene.hierarchy[newNode].parent == root,
		"cvol replay parent");
	Check(getNodeName(cluster->scene, int(newNode)) == "CVol_1",
		"cvol replay name");
	auto cvit = cluster->scene.cvolForNode.find(newNode);
	Check(cvit != cluster->scene.cvolForNode.end(), "cvol replay link");
	Check(cvit != cluster->scene.cvolForNode.end()
		&& cvit->second.priority == 2u
		&& cvit->second.spacing == 0.25f
		&& cvit->second.insideFade == 0.1f
		&& cvit->second.outsideFade == 0.2f,
		"cvol replay params (-ref spacing applied)");
	Check(cluster->scene.localTransform[newNode][3].x == 1.0f
		&& cluster->scene.localTransform[newNode][3].y == 2.0f,
		"cvol replay position");
	// Size attr rebuilt the scale channel (10 m box).
	glm::vec3 sk, tt, ss3;
	glm::vec4 pp;
	glm::quat qq;
	glm::decompose(cluster->scene.localTransform[newNode], ss3, qq,
		tt, sk, pp);
	Check(std::abs(ss3.x - 10.0f) < 1e-4f, "cvol replay size");

	// Unified form lowers to the legacy verb at parse.
	smvk::tour::Doc uni;
	Check(smvk::tour::LoadTourFromString(
		"createNode -at cvolume -p 0 -name \"U\" -t 0 0 0 1 0.5 0.1 0.1 -s 4 4 4;\n",
		uni, err), "cvol unified loads");
	Check(uni.commands.size() == 1
		&& uni.commands[0].verb == smvk::tour::Verb::CreateCVolume
		&& uni.commands[0].text == "U"
		&& uni.commands[0].vec4.x == 1.0f,
		"cvol unified lowers");
	// Type table knows the DAG kind.
	size_t tn = 0;
	const smvk::tour::CreateNodeType* t = smvk::tour::FindCreateNodeType("cvolume");
	Check(t && t->kind == smvk::tour::CreateNodeKind::Dag, "cvol node type");
	(void)tn;

	// Delete drops the def link (Maya delete: nothing bakes it).
	smvk::tour::Doc del;
	smvk::tour::Command dc{};
	dc.verb = smvk::tour::Verb::Delete;
	dc.node = newNode;
	dc.name = "CVol_1";
	del.commands.push_back(dc);
	Check(smvk::tour::ApplyTourToCluster(*cluster, del, &warns, &err),
		"cvol delete replay ok");
	Check(!cluster->scene.cvolForNode.contains(newNode),
		"cvol delete drops def");

	// Out-of-range -ref fails loudly; unknown attr fails at parse.
	smvk::tour::Doc bad;
	smvk::tour::Command bc{};
	bc.verb = smvk::tour::Verb::SetCvolAttr;
	bc.node = smvk::tour::kCreateCvolRefBase | 5u;
	bc.text = "spacing";
	bc.number = 1.0;
	bad.commands.push_back(bc);
	Check(!smvk::tour::ApplyTourToCluster(*cluster, bad, nullptr, &err),
		"cvol replay rejects bad ref");
	smvk::tour::Doc parseBad;
	Check(!smvk::tour::LoadTourFromString(
		"setCvolAttr -n 0 -at frobnicate -v 1;\n", parseBad, err),
		"cvol parser rejects bad attr");
	std::error_code ec;
	std::filesystem::remove(TempTour(), ec);
}

// Authored cubemap captures (pinned specular probes): createCubemap/
// setCubemapAttr round-trip, cook replay (node + node-local def,
// -ref addressing), delete drops the def, unified form lowers.
void TestCubemapVerbs() {
	smvk::tour::Doc doc;
	doc.source = "models/x.gltf";
	smvk::tour::Command c{};
	c.verb = smvk::tour::Verb::CreateCubemap;
	c.node = 0; // parent: cluster root
	c.text = "Cube_1";
	c.vec = glm::vec3(1.0f, 2.0f, 3.0f);
	c.number = 1.5; // influence radius
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetCubemapAttr;
	c.node = smvk::tour::kCreateCubemapRefBase | 0u;
	c.text = "radius";
	c.number = 2.5;
	doc.commands.push_back(c);

	std::string err;
	std::vector<std::string> warns;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "cube tour save");
	std::ifstream in(TempTour(), std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string text = ss.str();
	in.close();
	Check(text.find("createCubemap -p 0 -name \"Cube_1\"")
		!= std::string::npos, "cube legacy create line");
	Check(text.find("setCubemapAttr -ref 0 -at radius -v 2.5")
		!= std::string::npos, "cube tabulated attr line");
	smvk::tour::Doc back;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns),
		"cube tour load");
	Check(back.commands.size() == 2, "cube tour command count");
	Check(back.commands[0].verb == smvk::tour::Verb::CreateCubemap
		&& back.commands[0].text == "Cube_1"
		&& back.commands[0].number == 1.5, "cube create roundtrip");
	Check(back.commands[1].verb == smvk::tour::Verb::SetCubemapAttr
		&& back.commands[1].node == (smvk::tour::kCreateCubemapRefBase | 0u)
		&& back.commands[1].number == 2.5, "cube attr roundtrip");
	Check(!smvk::tour::Describe(back.commands[0]).empty()
		&& !smvk::tour::Describe(back.commands[1]).empty(),
		"cube describe");

	// Replay onto a cook cluster.
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	const int root = addNode(cluster->scene, -1, 0);
	setNodeName(cluster->scene, root, "Root");
	const size_t beforeNodes = cluster->scene.hierarchy.size();
	Check(smvk::tour::ApplyTourToCluster(*cluster, back, &warns, &err),
		"cube replay ok");
	Check(cluster->scene.hierarchy.size() == beforeNodes + 1,
		"cube replay appends node");
	const uint32_t newNode = uint32_t(beforeNodes);
	Check(getNodeName(cluster->scene, int(newNode)) == "Cube_1",
		"cube replay name");
	auto cmit = cluster->scene.cubemapForNode.find(newNode);
	Check(cmit != cluster->scene.cubemapForNode.end(), "cube replay link");
	Check(cmit != cluster->scene.cubemapForNode.end()
		&& cmit->second.radius == 2.5f,
		"cube replay params (-ref radius applied)");
	Check(cluster->scene.localTransform[newNode][3].y == 2.0f,
		"cube replay position");
	std::error_code ec;
	std::filesystem::remove(TempTour(), ec);

	// Unified form lowers; type table knows the DAG kind.
	smvk::tour::Doc uni;
	Check(smvk::tour::LoadTourFromString(
		"createNode -at cubemap -p 0 -name \"U\" -t 0 2 0 -v 1.5;\n",
		uni, err), "cube unified loads");
	Check(uni.commands.size() == 1
		&& uni.commands[0].verb == smvk::tour::Verb::CreateCubemap
		&& uni.commands[0].number == 1.5, "cube unified lowers");
	const smvk::tour::CreateNodeType* t =
		smvk::tour::FindCreateNodeType("cubemap");
	Check(t && t->kind == smvk::tour::CreateNodeKind::Dag,
		"cube node type");

	// Delete drops the def (nothing pins it).
	smvk::tour::Doc del;
	smvk::tour::Command dc{};
	dc.verb = smvk::tour::Verb::Delete;
	dc.node = newNode;
	dc.name = "Cube_1";
	del.commands.push_back(dc);
	Check(smvk::tour::ApplyTourToCluster(*cluster, del, &warns, &err),
		"cube delete replay ok");
	Check(!cluster->scene.cubemapForNode.contains(newNode),
		"cube delete drops def");

	// Out-of-range -ref fails loudly; unknown attr fails at parse.
	smvk::tour::Doc bad;
	smvk::tour::Command bc{};
	bc.verb = smvk::tour::Verb::SetCubemapAttr;
	bc.node = smvk::tour::kCreateCubemapRefBase | 5u;
	bc.text = "radius";
	bc.number = 1.0;
	bad.commands.push_back(bc);
	Check(!smvk::tour::ApplyTourToCluster(*cluster, bad, nullptr, &err),
		"cube replay rejects bad ref");
	smvk::tour::Doc parseBad;
	Check(!smvk::tour::LoadTourFromString(
		"setCubemapAttr -n 0 -at frobnicate -v 1;\n", parseBad, err),
		"cube parser rejects bad attr");
}

// Unified createNode form: new files write `createNode -at <type>`;
// DAG types lower to the legacy verbs at parse (single replay path);
// system types (material/file/image) parse, validate, and skip
// at replay; legacy createLight/createTerrain/createTransform lines
// load unchanged.
void TestCreateNode() {
	// Type table: DAG vs system knowledge lives on the type.
	size_t count = 0;
	Check(smvk::tour::CreateNodeTypes(count) != nullptr && count == 10,
		"createnode table size");
	const smvk::tour::CreateNodeType* t =
		smvk::tour::FindCreateNodeType("spotLight");
	Check(t && t->kind == smvk::tour::CreateNodeKind::Dag,
		"createnode spotLight is dag");
	Check(smvk::tour::FindCreateNodeType("material")
		&& smvk::tour::FindCreateNodeType("material")->kind
			== smvk::tour::CreateNodeKind::System,
		"createnode material is system");
	Check(smvk::tour::FindCreateNodeType("point") != nullptr,
		"createnode legacy alias point");
	Check(smvk::tour::FindCreateNodeType("nope") == nullptr,
		"createnode unknown type misses");
	// Writer emits the unified spelling per DAG type.
	smvk::tour::Doc doc;
	doc.source = "models/x.gltf";
	smvk::tour::Command c{};
	c.verb = smvk::tour::Verb::CreateNode;
	c.nodeType = "spotLight";
	c.node = 0;
	c.text = "Spot_1";
	c.vec = glm::vec3(1, 2, 3);
	c.vec4 = glm::vec4(2.0f, 100.0f, 10.0f, 15.0f);
	c.number = 30.0;
	doc.commands.push_back(c);
	smvk::tour::Command tr{};
	tr.verb = smvk::tour::Verb::CreateNode;
	tr.nodeType = "transform";
	tr.node = 0;
	tr.text = "Grp";
	tr.vec = glm::vec3(4, 5, 6);
	tr.vec4 = glm::vec4(0, 0, 0, 0.0f);
	tr.svec = glm::vec3(1, 2, 3);
	doc.commands.push_back(tr);
	smvk::tour::Command img{};
	img.verb = smvk::tour::Verb::CreateNode;
	img.nodeType = "image";
	img.node = 0;
	img.text = "Ref";
	doc.commands.push_back(img);
	std::string err;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "createnode save");
	std::ifstream in(TempTour(), std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string text = ss.str();
	in.close();
	Check(text.find("createNode -at spotLight -p 0 -name \"Spot_1\"")
		!= std::string::npos, "createnode light unified line");
	Check(text.find("createNode -at transform -p 0 -name \"Grp\"")
		!= std::string::npos, "createnode transform unified line");
	Check(text.find("createNode -at image -p 0 -name \"Ref\"")
		!= std::string::npos, "createnode system unified line");
	// Re-parse: DAG types lower to legacy verbs; system stays CreateNode.
	smvk::tour::Doc back;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns),
		"createnode reload");
	Check(back.commands.size() == 3, "createnode command count");
	if (back.commands.size() == 3) {
		Check(back.commands[0].verb == smvk::tour::Verb::CreateLight
			&& back.commands[0].text == "Spot_1"
			&& back.commands[0].vec4.y == 100.0f,
			"createnode light lowers");
		Check(back.commands[1].verb == smvk::tour::Verb::CreateTransform
			&& back.commands[1].text == "Grp"
			&& back.commands[1].svec.y == 2.0f,
			"createnode transform lowers");
		Check(back.commands[2].verb == smvk::tour::Verb::CreateNode
			&& back.commands[2].nodeType == "image",
			"createnode system stays");
	}
	// Cook replay: lowered light creates; system type warns+skips.
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	addNode(cluster->scene, -1, 0);
	Check(smvk::tour::ApplyTourToCluster(*cluster, back, &warns, &err),
		"createnode replay ok");
	Check(cluster->scene.lightForNode.size() == 1,
		"createnode replay creates light");
	Check(!warns.empty(), "createnode system replay reported");
	// Unknown -at fails the file loudly.
	smvk::tour::Doc parseBad;
	Check(!smvk::tour::LoadTourFromString(
		"createNode -at frobnicate -p 0 -name \"X\";\n", parseBad, err),
		"createnode parser rejects bad type");
	// Legacy spellings still load (backward compat).
	smvk::tour::Doc legacy;
	Check(smvk::tour::LoadTourFromString(
		"createLight -p 0 -name \"L\" -at point -t 0 0 0 10 5 0 0 0 0;\n",
		legacy, err), "createnode legacy loads");
	Check(legacy.commands.size() == 1
		&& legacy.commands[0].verb == smvk::tour::Verb::CreateLight,
		"createnode legacy verb kept");
	std::error_code ec;
	std::filesystem::remove(TempTour(), ec);
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

// Maya delete: round-trip in the .tour + cook replay drops the node's
// live links while ids stay addressable by later commands.
void TestDelete() {
	smvk::tour::Doc doc;
	smvk::tour::Command c{};
	c.verb = smvk::tour::Verb::Delete;
	c.node = 45;
	c.name = "Shop_Wall";
	doc.commands.push_back(c);
	std::string err;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "delete save");
	smvk::tour::Doc back;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns), "delete load");
	Check(back.commands.size() == 1 &&
		back.commands[0].verb == smvk::tour::Verb::Delete &&
		back.commands[0].node == 45, "delete round-trip");
	Check(smvk::tour::Describe(back.commands[0]).find("delete #45")
		!= std::string::npos, "delete describe");
	std::error_code ec;
	std::filesystem::remove(TempTour(), ec);

	// Cook replay: mesh/material/light links drop; the node itself (and
	// every later command addressing it) survives — ids stay stable.
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	const int root = addNode(cluster->scene, -1, 0);
	const int g0 = addNode(cluster->scene, root, 1);
	const int g1 = addNode(cluster->scene, root, 1);
	setNodeName(cluster->scene, g0, "Wall");
	setNodeName(cluster->scene, g1, "Floor");
	smvk::MaterialCreateInfo ci{};
	ci.baseColorFactor = glm::vec4(1.0f);
	const smvk::ClusterMaterialHandle m0 = cluster->CreateMaterial(ci);
	cluster->scene.meshForNode[uint32_t(g0)] = 0;
	cluster->scene.materialForNode[uint32_t(g0)] = m0.index();
	cluster->scene.materialForNode[uint32_t(g1)] = m0.index();
	::SceneLight def{};
	def.type = ::SceneLight::Type::Point;
	def.name = "L";
	const uint32_t lIdx =
		cluster->lights.create(std::move(def)).index();
	cluster->scene.lightForNode[uint32_t(g0)] = lIdx;

	smvk::tour::Doc doc2;
	smvk::tour::Command d{};
	d.verb = smvk::tour::Verb::Delete;
	d.node = uint32_t(g0);
	d.name = "Wall";
	doc2.commands.push_back(d);
	smvk::tour::Command r2{};
	r2.verb = smvk::tour::Verb::Rename; // ids stay addressable after
	r2.node = uint32_t(g0);             // the delete (Maya parity)
	r2.text = "Deleted_Wall";
	doc2.commands.push_back(r2);
	Check(smvk::tour::ApplyTourToCluster(*cluster, doc2, &warns, &err),
		"delete replay ok");
	Check(!cluster->scene.meshForNode.contains(uint32_t(g0)),
		"delete drops mesh link");
	Check(!cluster->scene.materialForNode.contains(uint32_t(g0)),
		"delete drops material link");
	Check(!cluster->scene.lightForNode.contains(uint32_t(g0)),
		"delete drops light link");
	Check(getNodeName(cluster->scene, g0) == "Deleted_Wall",
		"delete keeps ids stable");
	Check(cluster->scene.materialForNode[uint32_t(g1)] == m0.index(),
		"delete leaves siblings alone");

	smvk::tour::Doc bad;
	smvk::tour::Command bd{};
	bd.verb = smvk::tour::Verb::Delete;
	bd.node = 9999;
	bad.commands.push_back(bd);
	Check(!smvk::tour::ApplyTourToCluster(*cluster, bad, nullptr, &err),
		"delete rejects bad node");
}

// Maya's four identity layers on one scene: MUuid (persistent, unique),
// MObject (runtime numeric id), MDagPath (full + shortest-unique), and
// human name+namespace addressing.
void TestIdentityLayers() {
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	const int root = addNode(cluster->scene, -1, 0); // unnamed root
	const int grpA = addNode(cluster->scene, root, 1);
	const int grpB = addNode(cluster->scene, root, 1);
	const int meshA = addNode(cluster->scene, grpA, 2);
	const int meshB = addNode(cluster->scene, grpB, 2);
	const int solo = addNode(cluster->scene, grpB, 2);
	setNodeName(cluster->scene, grpA, "grpA");
	setNodeName(cluster->scene, grpB, "grpB");
	setNodeName(cluster->scene, meshA, "Mesh");
	setNodeName(cluster->scene, meshB, "Mesh");
	setNodeName(cluster->scene, solo, "Solo");
	Scene& scene = cluster->scene;
	// MDagPath: full path (unnamed root contributes nothing)...
	Check(smvk::tour::DagFullPath(scene, uint32_t(meshA)) == "grpA/Mesh",
		"dag full path");
	Check(smvk::tour::DagFullPath(scene, uint32_t(root)).empty(),
		"dag full path root empty");
	// ...and shortest unique path (bare name when it already is unique).
	Check(smvk::tour::DagShortestUniquePath(scene, uint32_t(meshA))
		== "grpA/Mesh", "dag shortest duplicated");
	Check(smvk::tour::DagShortestUniquePath(scene, uint32_t(solo))
		== "Solo", "dag shortest unique-name");
	Check(smvk::tour::DagShortestUniquePath(scene, uint32_t(grpA))
		== "grpA", "dag shortest group");
	uint32_t hit = UINT32_MAX;
	Check(smvk::tour::ResolveDagPath(scene, "grpB/Mesh", hit)
		&& hit == uint32_t(meshB), "dag resolve full");
	Check(!smvk::tour::ResolveDagPath(scene, "Mesh", hit),
		"dag resolve bare-name rejected");
	Check(!smvk::tour::ResolveDagPath(scene, "nope/x", hit),
		"dag resolve miss");
	// MUuid: setUuid assigns; re-setting the holder is a no-op true;
	// handing another live node's uuid to anyone is rejected.
	const std::string hexA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	const std::string hexB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	Check(smvk::tour::SetNodeUuid(scene, uint32_t(meshA), hexA),
		"uuid first assign");
	Check(smvk::tour::SetNodeUuid(scene, uint32_t(meshA), hexA),
		"uuid same-holder idempotent");
	Check(!smvk::tour::SetNodeUuid(scene, uint32_t(meshB), hexA),
		"uuid duplicate rejected");
	Check(scene.uuidForNode[uint32_t(meshA)] == hexA,
		"uuid holder keeps identity");
	Check(smvk::tour::SetNodeUuid(scene, uint32_t(meshB), hexB),
		"uuid other node ok");
	Check(!smvk::tour::SetNodeUuid(scene, 9999, hexA),
		"uuid out-of-range rejected");
	Check(!smvk::tour::SetNodeUuid(scene, uint32_t(meshA), ""),
		"uuid empty rejected");
	// Cook level: a duplicated uuid line aborts strict replays and is
	// reported+skipped in non-strict (reference) replays.
	{
		smvk::tour::Doc doc;
		smvk::tour::Command u1{};
		u1.verb = smvk::tour::Verb::SetUuid;
		u1.node = uint32_t(solo);
		u1.uuid = "cccccccccccccccccccccccccccccccc";
		doc.commands.push_back(u1);
		smvk::tour::Command u2 = u1;
		u2.node = uint32_t(grpA); // same hex, other node
		doc.commands.push_back(u2);
		std::string err;
		Check(!smvk::tour::ApplyTourToCluster(*cluster, doc, nullptr,
			&err), "uuid duplicate strict aborts");
		std::vector<std::string> warns;
		Check(smvk::tour::ApplyTourToCluster(*cluster, doc, &warns,
			nullptr, false), "uuid duplicate non-strict survives");
		Check(!warns.empty(), "uuid duplicate non-strict reported");
	}
	// Delete through the layered resolver: uuid hit with a stale id
	// AND a renamed node (uuids survive renames and hierarchy moves).
	{
		setNodeName(scene, meshA, "RenamedMesh");
		cluster->scene.meshForNode[uint32_t(meshA)] = 0;
		smvk::tour::Doc doc;
		smvk::tour::Command d{};
		d.verb = smvk::tour::Verb::Delete;
		d.node = 9999; // stale id AND stale name on purpose
		d.name = "Mesh";
		d.uuid = hexA;
		doc.commands.push_back(d);
		std::string err;
		std::vector<std::string> warns;
		Check(smvk::tour::ApplyTourToCluster(*cluster, doc, &warns, &err),
			"uuid delete after rename");
		Check(!cluster->scene.meshForNode.contains(uint32_t(meshA)),
			"uuid delete unlinks renamed node");
	}
}

// Maya delete addressing: uuid primary, name secondary, id fallback.
void TestDeleteAddressing() {
	auto freshCluster = []() {
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetCookMode(true);
		addNode(cluster->scene, -1, 0); // root (unnamed)
		return cluster;
	};
	auto cluster = freshCluster();
	// root/grpA + root/grpB, each with a "Mesh" child (duplicated name).
	const int grpA = addNode(cluster->scene, 0, 1);
	const int grpB = addNode(cluster->scene, 0, 1);
	const int meshA = addNode(cluster->scene, grpA, 2);
	const int meshB = addNode(cluster->scene, grpB, 2);
	setNodeName(cluster->scene, grpA, "grpA");
	setNodeName(cluster->scene, grpB, "grpB");
	setNodeName(cluster->scene, meshA, "Mesh");
	setNodeName(cluster->scene, meshB, "Mesh");
	cluster->scene.meshForNode[uint32_t(meshA)] = 0;
	cluster->scene.meshForNode[uint32_t(meshB)] = 0;
	std::vector<std::string> warns;
	std::string err;

	// Writer form: delete -uuid <hex> -n <id> -name "<name>".
	smvk::tour::Doc doc;
	smvk::tour::Command u{};
	u.verb = smvk::tour::Verb::SetUuid;
	u.node = uint32_t(meshA);
	u.uuid = "0123456789abcdef0123456789abcdef";
	doc.commands.push_back(u);
	smvk::tour::Command d{};
	d.verb = smvk::tour::Verb::Delete;
	d.node = uint32_t(meshA);
	d.name = "Mesh";
	d.uuid = "0123456789abcdef0123456789abcdef";
	doc.commands.push_back(d);
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "uuid save");
	smvk::tour::Doc back;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns),
		"uuid load");
	Check(back.commands.size() == 2
		&& back.commands[0].verb == smvk::tour::Verb::SetUuid
		&& back.commands[0].uuid == d.uuid
		&& back.commands[1].verb == smvk::tour::Verb::Delete
		&& back.commands[1].uuid == d.uuid
		&& back.commands[1].name == "Mesh", "uuid round-trip");
	Check(smvk::tour::ApplyTourToCluster(*cluster, back, &warns, &err),
		"uuid replay ok");
	// meshA unlinked through its uuid (the STALE numeric id 9999 in the
	// variant below must not matter); meshB untouched.
	Check(!cluster->scene.meshForNode.contains(uint32_t(meshA)),
		"uuid delete unlinks");
	Check(cluster->scene.meshForNode.contains(uint32_t(meshB)),
		"uuid delete spares siblings");

	// Stale id + valid uuid: uuid wins even when -n points elsewhere.
	{
		smvk::tour::Doc doc2;
		smvk::tour::Command d2{};
		d2.verb = smvk::tour::Verb::Delete;
		d2.node = 9999;
		d2.uuid = "fedcba9876543210fedcba9876543210";
		doc2.commands.push_back(d2);
		cluster->scene.uuidForNode[uint32_t(meshB)] = d2.uuid;
		Check(smvk::tour::ApplyTourToCluster(*cluster, doc2, &warns, &err),
			"uuid wins over stale id");
		Check(!cluster->scene.meshForNode.contains(uint32_t(meshB)),
			"uuid delete hits by uuid");
	}
	// DAG path disambiguates duplicated basenames: grpA/Mesh.
	{
		cluster->scene.meshForNode[uint32_t(meshA)] = 0;
		cluster->scene.meshForNode[uint32_t(meshB)] = 0;
		smvk::tour::Doc doc3;
		smvk::tour::Command d3{};
		d3.verb = smvk::tour::Verb::Delete;
		d3.node = 9999; // stale on purpose
		d3.name = "grpA/Mesh";
		doc3.commands.push_back(d3);
		Check(smvk::tour::ApplyTourToCluster(*cluster, doc3, &warns, &err),
			"path delete ok");
		Check(!cluster->scene.meshForNode.contains(uint32_t(meshA)),
			"path delete hits right leaf");
		Check(cluster->scene.meshForNode.contains(uint32_t(meshB)),
			"path delete spares other leaf");
	}
	// Unique-name delete (no uuid, no useful id): name resolves.
	// (meshB gets a unique name first; per-node delete never touches
	// anything but the target's own links.)
	{
		setNodeName(cluster->scene, meshB, "Solo");
		cluster->scene.meshForNode[uint32_t(meshB)] = 0;
		smvk::tour::Doc doc4;
		smvk::tour::Command d4{};
		d4.verb = smvk::tour::Verb::Delete;
		d4.node = 9999;
		d4.name = "Solo";
		doc4.commands.push_back(d4);
		Check(smvk::tour::ApplyTourToCluster(*cluster, doc4, &warns, &err),
			"name delete ok");
		Check(!cluster->scene.meshForNode.contains(uint32_t(meshB)),
			"name delete unlinks");
	}
	// Legacy nameless delete still resolves by id.
	{
		cluster->scene.meshForNode[uint32_t(meshB)] = 0;
		smvk::tour::Doc doc5;
		smvk::tour::Command d5{};
		d5.verb = smvk::tour::Verb::Delete;
		d5.node = uint32_t(meshB);
		doc5.commands.push_back(d5);
		Check(smvk::tour::ApplyTourToCluster(*cluster, doc5, &warns, &err),
			"legacy delete ok");
		Check(!cluster->scene.meshForNode.contains(uint32_t(meshB)),
			"legacy delete unlinks");
	}
	// GenerateUuid: 32 lowercase hex, unique across calls.
	{
		const std::string a = smvk::tour::GenerateUuid();
		const std::string b = smvk::tour::GenerateUuid();
		bool hex = a.size() == 32 && b.size() == 32;
		for (char ch : a + b)
			hex = hex && ((ch >= '0' && ch <= '9')
				|| (ch >= 'a' && ch <= 'f'));
		Check(hex && a != b, "uuid generates unique hex");
	}
	std::error_code ec;
	std::filesystem::remove(TempTour(), ec);
}

void TestFileNodes() {
	smvk::tour::Doc doc;
	smvk::tour::FileNode fn;
	fn.name = "Brick \"Wall\"";
	fn.path = "textures/brick wall.png";
	fn.colorspace = 0;
	fn.wrap = 1;
	fn.filter = 1;
	doc.fileNodes.push_back(fn);
	fn = smvk::tour::FileNode{};
	fn.name = "rough";
	fn.path = "F:/content/models/x/r.png";
	fn.colorspace = 1;
	fn.wrap = 2;
	fn.filter = 0;
	doc.fileNodes.push_back(fn);
	doc.hasCook = true;
	doc.cook.autoKtx = false;
	std::string err;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "filenode save");
	smvk::tour::Doc back;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns), "filenode load");
	Check(back.fileNodes.size() == 2, "filenode count");
	if (back.fileNodes.size() == 2) {
		Check(back.fileNodes[0].name == "Brick \"Wall\""
			&& back.fileNodes[0].path == "textures/brick wall.png"
			&& back.fileNodes[0].colorspace == 0
			&& back.fileNodes[0].wrap == 1
			&& back.fileNodes[0].filter == 1, "filenode srgb clamp nearest");
		Check(back.fileNodes[1].name == "rough"
			&& back.fileNodes[1].path == "F:/content/models/x/r.png"
			&& back.fileNodes[1].colorspace == 1
			&& back.fileNodes[1].wrap == 2
			&& back.fileNodes[1].filter == 0, "filenode linear mirrored linear");
	}
	Check(back.hasCook && back.cook.autoKtx == false, "filenode cook autoktx");
	// Numeric tolerance + missing-path skip via hand-written lines.
	{
		std::ofstream f(TempTour(), std::ios::binary | std::ios::trunc);
		f << "fileNode -name \"n\" -path \"p.png\" -colorspace 1 -wrap 0 -filter 0;\n";
		f << "fileNode -name \"noop\";\n";
		f.close();
		smvk::tour::Doc h;
		std::vector<std::string> w2;
		Check(smvk::tour::LoadTour(TempTour(), h, err, &w2), "filenode hand load");
		Check(h.fileNodes.size() == 1 && h.fileNodes[0].colorspace == 1,
			"filenode numeric + pathless skip");
	}
	std::error_code ec;
	std::filesystem::remove(TempTour(), ec);
}

void TestCamera() {
	// Hand-built doc: full-lens create + -ref attrs (fov + position).
	smvk::tour::Doc doc;
	smvk::tour::Command c{};
	c.verb = smvk::tour::Verb::CreateCamera;
	c.node = 0;
	c.text = "Shot_1";
	c.vec = glm::vec3(1.0f, 2.0f, 3.0f);
	c.svec = glm::vec3(10.0f, 20.0f, 30.0f);
	c.vec4 = glm::vec4(0.0f, 35.0f, 0.5f, 500.0f);
	c.number = 10.0;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetCamAttr;
	c.node = smvk::tour::kCreateCameraRefBase | 0u;
	c.text = "fovY";
	c.number = 40.0;
	doc.commands.push_back(c);
	c = smvk::tour::Command{};
	c.verb = smvk::tour::Verb::SetCamAttr;
	c.node = smvk::tour::kCreateCameraRefBase | 0u;
	c.text = "position";
	c.vec4 = glm::vec4(4.0f, 5.0f, 6.0f, 1.0f);
	doc.commands.push_back(c);
	std::string err;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "camera save");
	{
		std::ifstream in(TempTour(), std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		const std::string text = ss.str();
		Check(text.find("createCamera -p 0 -name \"Shot_1\"") != std::string::npos,
			"camera create line");
		Check(text.find("\tsetCamAttr -ref 0 -at fovY") != std::string::npos,
			"camera tabulated ref");
	}
	smvk::tour::Doc back;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns), "camera load");
	Check(back.commands.size() == 3, "camera command count");
	if (back.commands.size() == 3) {
		const auto& cc = back.commands[0];
		Check(cc.verb == smvk::tour::Verb::CreateCamera
			&& cc.text == "Shot_1" && cc.vec.x == 1.0f
			&& cc.svec.y == 20.0f && cc.vec4.y == 35.0f,
			"camera create payload");
		Check(back.commands[1].verb == smvk::tour::Verb::SetCamAttr
			&& back.commands[1].text == "fovY"
			&& back.commands[1].number == 40.0, "camera fov attr");
		Check(back.commands[2].text == "position"
			&& back.commands[2].vec4.x == 4.0f, "camera position attr");
	}
	// Unified spelling lowers identically.
	{
		std::ofstream f(TempTour(), std::ios::binary | std::ios::trunc);
		f << "createNode -at camera -p 0 -name \"U\" -t 0 1 2 0 0 0 1 35 0.5 500 8;\n";
		f.close();
		smvk::tour::Doc u;
		Check(smvk::tour::LoadTour(TempTour(), u, err, nullptr), "camera unified load");
		Check(u.commands.size() == 1
			&& u.commands[0].verb == smvk::tour::Verb::CreateCamera
			&& u.commands[0].vec4.x == 1.0f
			&& u.commands[0].number == 8.0, "camera unified lowers");
	}
	// Lens validation rejects garbage.
	{
		std::ofstream f(TempTour(), std::ios::binary | std::ios::trunc);
		f << "createCamera -p 0 -name \"B\" -t 0 0 0 0 0 0 0 0 0.5 500 8;\n";
		f.close();
		smvk::tour::Doc b;
		Check(!smvk::tour::LoadTour(TempTour(), b, err, nullptr), "camera bad lens rejected");
		std::ofstream f2(TempTour(), std::ios::binary | std::ios::trunc);
		f2 << "setCamAttr -n 1 -at zoom -v 2;\n";
		f2.close();
		smvk::tour::Doc b2;
		Check(!smvk::tour::LoadTour(TempTour(), b2, err, nullptr), "camera bad attr rejected");
	}
	// Replay: node + pool def + link + -ref attr + position.
	{
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetCookMode(true);
		addNode(cluster->scene, -1, 0);
		std::vector<std::string> rw;
		std::string rerr;
		Check(smvk::tour::ApplyTourToCluster(*cluster, back, &rw, &rerr),
			"camera replay ok");
		Check(cluster->scene.hierarchy.size() == 2, "camera replay node");
		const auto it = cluster->scene.cameraForNode.find(1);
		Check(it != cluster->scene.cameraForNode.end(), "camera replay link");
		if (it != cluster->scene.cameraForNode.end()) {
			const smvk::ClusterCameraHandle h =
				cluster->cameras.getHandle(it->second);
			const SceneCamera* def = cluster->cameras.get(h);
			Check(def && def->fovYDeg == 40.0f
				&& def->projection == SceneCamera::Projection::Perspective
				&& def->name == "Shot_1", "camera replay lens+name");
		}
		// Position channel rebuilt the translate (rotation stayed 10/20/30).
		const glm::vec3 t(cluster->scene.localTransform[1][3]);
		Check(std::abs(t.x - 4.0f) < 1e-4f && std::abs(t.y - 5.0f) < 1e-4f
			&& std::abs(t.z - 6.0f) < 1e-4f, "camera replay position");
	}
	std::error_code ec;
	std::filesystem::remove(TempTour(), ec);
}

// Artifact-cache consultation through the importer's texture path: a
// hook sees Get-before-transcode, receives the cooked payload via Put
// on miss, serves it byte-identically on the next import (no second
// Put), and a corrupt cached payload reprocesses (never trusted).
void TestTextureCacheConsult() {
	namespace fs = std::filesystem;
	const auto dir = fs::temp_directory_path() / "vulkantoy_texcachetest";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	// Hand-written minimal 1x1 32-bit uncompressed TGA (stb-readable).
	{
		const unsigned char tga[22] = {
			0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			1, 0, 1, 0, 32, 0, // 1x1, 32bpp, bottom-up
			0x00, 0x00, 0xFF, 0xFF // BGRA: opaque blue
		};
		std::ofstream f(dir / "tex.tga", std::ios::binary);
		f.write(reinterpret_cast<const char*>(tga), sizeof(tga));
	}
	// glTF: 1 triangle (36 zero bytes via data-URI buffer), material 0
	// with a baseColorTexture -> texture 0 -> image tex.tga.
	// 36 zero bytes -> 48 base64 chars (12 groups of "AAAA"); built
	// programmatically so the count cannot drift.
	std::string b64;
	for (int i = 0; i < 12; ++i)
		b64 += "AAAA";
	const std::string gltfText =
		"{\"asset\":{\"version\":\"2.0\"},"
		"\"scenes\":[{\"nodes\":[0]}],"
		"\"nodes\":[{\"mesh\":0}],"
		"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
		"\"material\":0}]}],"
		"\"materials\":[{\"pbrMetallicRoughness\":"
		"{\"baseColorTexture\":{\"index\":0}}}],"
		"\"textures\":[{\"source\":0}],"
		"\"images\":[{\"uri\":\"tex.tga\"}],"
		"\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
		"\"count\":3,\"type\":\"VEC3\",\"max\":[0,0,0],\"min\":[0,0,0]}],"
		"\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
		"\"buffers\":[{\"byteLength\":36,"
		"\"uri\":\"data:application/octet-stream;base64,"
		+ b64 + "\"}]}";
	const auto gltfPath = dir / "tex.gltf";
	{
		std::ofstream f(gltfPath, std::ios::binary | std::ios::trunc);
		f << gltfText;
	}
	// Fake cache: records every call; Get's payload is injectable.
	struct FakeCache final : GltfArtifactCache {
		int getCalls = 0;
		int putCalls = 0;
		std::string lastHash;
		std::string lastMeta;
		std::vector<uint8_t> served; // empty = miss
		bool Get(const std::string&, const std::string& uri,
			std::vector<uint8_t>& out) override {
			(void)uri;
			++getCalls;
			if (served.empty())
				return false;
			out = served;
			return true;
		}
		bool Put(const std::string&, const std::string&,
			const void* payload, size_t payloadSize,
			const std::string& sourceHash,
			const std::string& meta) override {
			++putCalls;
			lastHash = sourceHash;
			lastMeta = meta;
			const auto* p = static_cast<const uint8_t*>(payload);
			served.assign(p, p + payloadSize);
			return true;
		}
	};
	auto importWith = [&](FakeCache& cache) -> bool {
		tinygltf::Model model;
		if (!LoadModelFromAsciiFile(model, gltfPath.string().c_str()))
			return false;
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetCookMode(true);
		addNode(cluster->scene, -1, 0);
		GltfToSceneImporter importer(*cluster, model, gltfPath);
		importer.setTextureSource(
			std::make_shared<HostTextureSource>(dir));
		importer.setArtifactCache(&cache);
		importer.import(0);
		return cluster->scene.meshForNode.size() == 1
			&& cluster->textures.numObjects() >= 0;
	};

	FakeCache cache;
	Check(importWith(cache), "texcache import ok");
	Check(cache.getCalls == 1, "texcache consulted once");
	Check(cache.putCalls == 1, "texcache staged once on miss");
	Check(!cache.lastHash.empty() && cache.lastHash.size() == 16,
		"texcache hash key");
	// The staged payload is a real KTX2 (the transcode output).
	Check(cache.served.size() >= 12 && cache.served[0] == 0xAB
		&& cache.served[4] == ' ' && cache.served[5] == '2',
		"texcache payload is ktx2");

	// Second import with the served payload: HIT — no transcode, no
	// Put. (The first import wrote a .ktx2 sidecar next to the source;
	// remove it so the store consult is actually reached.)
	fs::remove(dir / "tex.ktx2", ec);
	FakeCache hit;
	hit.served = cache.served;
	Check(importWith(hit), "texcache hit import ok");
	Check(hit.getCalls == 1 && hit.putCalls == 0,
		"texcache hit serves without re-staging");

	// Corrupt cached payload: never trusted — reprocess + re-stage.
	fs::remove(dir / "tex.ktx2", ec);
	FakeCache corrupt;
	corrupt.served = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
	Check(importWith(corrupt), "texcache corrupt import ok");
	Check(corrupt.putCalls == 1,
		"texcache corrupt reprocesses + restages");

	fs::remove_all(dir, ec);
}

} // namespace

// Reference asset GUIDs: `reference ... -guid <32-hex>` round-trips, and
// the GltfGuidResolver hook resolves identity-FIRST (a moved source file
// keeps binding through its GUID even when the recorded -source path is
// stale; a miss falls back to the recorded path exactly as before).
void TestReferenceGuid() {
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "vulkantoy_refguid";
	std::error_code ec;
	fs::create_directories(dir, ec);
	// The referenced source lives at a NEW path; the tour still records
	// the OLD one (a rename that happened outside the editor).
	const fs::path gltf = dir / "moved.gltf";
	{
		std::ofstream f(gltf, std::ios::binary | std::ios::trunc);
		f << "{\"asset\":{\"version\":\"2.0\"},"
			"\"scenes\":[{\"nodes\":[0]}],"
			"\"nodes\":[{\"name\":\"Solo\"}]}";
	}
	const std::string kGuid = "0123456789abcdef0123456789abcdef";
	// Round-trip: emit + parse keep the guid beside the source.
	smvk::tour::Doc doc;
	smvk::tour::ReferenceCmd r{};
	r.parent = 0;
	r.source = "old_name.gltf"; // stale (the file was renamed)
	r.guid = kGuid;
	r.name = "solo";
	doc.references.push_back(r);
	const std::string tourPath = (dir / "def.tour").string();
	std::string err;
	Check(smvk::tour::SaveTour(tourPath, doc, err), "refguid save");
	{
		std::ifstream f(tourPath, std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		const std::string text = ss.str();
		Check(text.find("-guid " + kGuid) != std::string::npos,
			"refguid emitted");
	}
	smvk::tour::Doc back;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(tourPath, back, err, &warns),
		"refguid load");
	Check(back.references.size() == 1
		&& back.references[0].guid == kGuid
		&& back.references[0].source == "old_name.gltf",
		"refguid roundtrip");
	// Resolver: GUID -> the moved file's path.
	struct FakeResolver final : public GltfGuidResolver {
		std::string movedTo;
		std::string PathForGuid(const std::string& guid) override {
			if (guid == "0123456789abcdef0123456789abcdef")
				return movedTo;
			return {}; // unknown: caller falls back to -source
		}
	};
	FakeResolver resolver;
	resolver.movedTo = gltf.generic_string();
	// Identity-first: stale -source + GUID resolves to the moved file.
	{
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetCookMode(true);
		addNode(cluster->scene, -1, 0);
		std::vector<int> ph;
		std::vector<std::string> issues;
		const size_t applied = ApplySceneReferences(*cluster, back,
			dir.generic_string(), nullptr, nullptr, nullptr,
			smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0, &ph,
			&issues, nullptr, true, nullptr, nullptr, &resolver);
		Check(applied == 1, "refguid identity-first bind");
	}
	// Resolver miss ("" = unknown GUID): falls back to the recorded
	// path — which is stale here, so the reference skips with an issue
	// (never a crash, never wrong geometry).
	{
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetCookMode(true);
		addNode(cluster->scene, -1, 0);
		std::vector<int> ph;
		std::vector<std::string> issues;
		const size_t applied = ApplySceneReferences(*cluster, back,
			dir.generic_string(), nullptr, nullptr, nullptr,
			smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0, &ph,
			&issues, nullptr, true, nullptr, nullptr, nullptr);
		Check(applied == 0 && !issues.empty(),
			"refguid miss falls back + skips cleanly");
	}
}

// Material-asset pool imports: round-trip as ordered materialAsset lines
// (assets + the editor-default seed), so reload/cook rebuild the identical
// cluster pool and the tour's assignMaterial/-m indices resolve exactly
// as authored (the failure this pins: an editor session minted material 26
// from an asset-browser .material; the cook had no record, cooked a
// 26-entry pool, and SerializeCluster failed with "scene references
// unknown material pool index 26").
void TestMaterialAssets() {
	smvk::tour::Doc doc;
	smvk::tour::MaterialAssetCmd def{};
	def.editorDefault = true;
	def.name = "default";
	doc.materialAssets.push_back(def);
	smvk::tour::MaterialAssetCmd asset{};
	asset.path = "content/models/Sponza/mat0_brick.material";
	asset.name = "Brick \"Rough\"";
	doc.materialAssets.push_back(asset);
	asset = smvk::tour::MaterialAssetCmd{};
	asset.path = "F:/projects/game/content/models/Sponza/mat1_leaf.material";
	asset.name = "leaf";
	doc.materialAssets.push_back(asset);
	std::string err;
	Check(smvk::tour::SaveTour(TempTour(), doc, err), "materialasset save");
	smvk::tour::Doc back;
	std::vector<std::string> warns;
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns),
		"materialasset load");
	Check(back.materialAssets.size() == 3, "materialasset count + order");
	if (back.materialAssets.size() == 3) {
		Check(back.materialAssets[0].editorDefault
			&& back.materialAssets[0].path.empty()
			&& back.materialAssets[0].name == "default",
			"materialasset default seed");
		Check(!back.materialAssets[1].editorDefault
			&& back.materialAssets[1].path
				== "content/models/Sponza/mat0_brick.material"
			&& back.materialAssets[1].name == "Brick \"Rough\"",
			"materialasset project-relative record");
		Check(!back.materialAssets[2].editorDefault
			&& back.materialAssets[2].path
				== "F:/projects/game/content/models/Sponza/mat1_leaf.material",
			"materialasset absolute record");
	}
	// Hand-written forms: bare -default; (no space), missing -path/-default
	// warns + skips, a lone -path record parses (name optional).
	{
		std::ofstream f(TempTour(), std::ios::binary | std::ios::trunc);
		f << "materialAsset -default;\n"
			"materialAsset -name \"no path\";\n"
			"materialAsset -path \"content/x.material\";\n";
	}
	back = smvk::tour::Doc{};
	warns.clear();
	Check(smvk::tour::LoadTour(TempTour(), back, err, &warns),
		"materialasset hand-written load");
	Check(back.materialAssets.size() == 2
		&& back.materialAssets[0].editorDefault
		&& !back.materialAssets[1].editorDefault
		&& back.materialAssets[1].path == "content/x.material"
		&& back.materialAssets[1].name.empty(),
		"materialasset bare -default + path-only record");
	Check(!warns.empty(), "materialasset missing path warns");
}

int main() {
	TestRoundTrip();
	TestFileNodes();
	TestCamera();
	TestTextureCacheConsult();
	TestTourFormat();
	TestUnknownVerbSkipped();
	TestReparent();
	TestDelete();
	TestDeleteAddressing();
	TestIdentityLayers();
	TestReplay();
	TestReferences();
	TestTourReferences();
	TestLightVerbs();
	TestTerrainVerbs();
	TestCvolVerbs();
	TestCubemapVerbs();
	TestRootReference();
	TestReferenceGuid();
	TestCreateNode();
	TestMaterialAssets();
	std::cout << g_checks - g_failures << "/" << g_checks << " passed\n";
	return g_failures == 0 ? 0 : 1;
}
