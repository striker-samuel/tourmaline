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
	c.verb = smvk::tour::Verb::Parent;
	c.node = 45;
	c.node2 = 12;
	doc.commands.push_back(c);
	doc.hasCook = true;
	doc.cook.outPak = "content/paks/x.pak";
	doc.cook.lightmap = true;
	doc.cook.lmDensity = 24;
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
	Check(back.cook.lightmap && back.cook.lmDensity == 24, "tour cook bake");
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
		Check(back.commands[9].node2 == 12, "tour parent");
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
	c.verb = smvk::tour::Verb::Parent;
	c.node = uint32_t(g0);
	c.node2 = uint32_t(g1);
	doc.commands.push_back(c);

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

} // namespace

int main() {
	TestRoundTrip();
	TestUnknownVerbSkipped();
	TestReparent();
	TestReplay();
	std::cout << g_checks - g_failures << "/" << g_checks << " passed\n";
	return g_failures == 0 ? 0 : 1;
}
