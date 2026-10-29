// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// test_knight_anim: end-to-end knight walk-clip diagnostic on the REAL
// derived assets (F:/content/demo/knight_only): the .geometry rebuild
// (BuildFromMeshAsset — the exact path the pak cluster went through) +
// the .anim0_* clip attach + the Cluster pose driver, then:
//   1. the runtime node-id contract (the .anim's -joint ids are the
//      derivation cluster ids; the names must resolve to the SAME ids),
//   2. the joint HIERARCHY under the pelvis (the hip-flap chain must
//      stay parented: pelvis -> hipSwinger -> flaps),
//   3. the follow test: as the pelvis walks away from the bind pose, the
//      hip joints' WORLD positions must stay near the pelvis (an
//      anatomically constant offset), not stay at the bind location.
// SKIPs (exit 0) when the knight content is absent — no F:/content, no
// test. GPU-free (offline cluster mode).

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <Engine/App/cluster.h>
#include <Engine/Asset/touranim.h>
#include <Engine/Scene/scene.h>

#include <gltf_import/gltf.h>
#include <gltf_import/asset_export.h>

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

std::string NodeName(const Scene& s, int n) {
	return (n >= 0 && n < int(s.hierarchy.size()))
		? getNodeName(s, n) : "<none>";
}

// Walks n's ancestor chain (nearest first), "id:name" pairs.
std::string ParentChain(const Scene& s, int n) {
	std::ostringstream o;
	int cur = (n >= 0 && n < int(s.hierarchy.size()))
		? s.hierarchy[size_t(n)].parent : -1;
	int guard = 0;
	while (cur >= 0 && guard++ < 64) {
		o << cur << ":" << getNodeName(s, cur);
		if (cur < int(s.hierarchy.size()))
			cur = s.hierarchy[size_t(cur)].parent;
		else
			cur = -1;
		if (cur >= 0)
			o << " <- ";
	}
	return o.str();
}

} // namespace

int main() {
	namespace fs = std::filesystem;
	const fs::path dir = "F:/content/demo/knight_only";
	const fs::path geo = dir / "knight.geometry";
	const fs::path anim = dir
		/ "knight.anim0_knight_rig_root_Take_001_BaseLayer.anim";
	if (!fs::exists(geo) || !fs::exists(anim)) {
		std::cout << "SKIP knight content absent (" << dir.string()
			<< ")\n";
		return 0;
	}

	// ---- 1) The .geometry rebuild (BuildFromMeshAsset, ph=0) ----------
	std::string err;
	smvk::assets::MeshAsset asset;
	if (!smvk::assets::LoadMeshAsset(geo, asset, err)) {
		std::cout << "FAIL LoadMeshAsset: " << err << "\n";
		return 1;
	}
	std::cout << "[knight] geometry: " << asset.nodes.size() << " nodes, "
		<< asset.geometry.size() << " blobs, skeleton "
		<< (asset.hasSkeleton ? "yes" : "no") << " ("
		<< asset.skeleton.joints.size() << " joints, root asset-node "
		<< asset.skeleton.root << ")\n";
	auto c = std::make_unique<smvk::Cluster>(nullptr);
	c->SetOfflineMode(true);
	// The COOK's definition-build shape (cook main: "empty scene root,
	// then the reference bind"): node 0 exists BEFORE BuildFromMeshAsset
	// grafts the artifact under it (ph = 0 = the scene root). Without it
	// every runtime id shifts down one against the .anim's -joint ids.
	addNode(c->scene, -1, 0);
	smvk::assets::AssetManifest manifest{};
	const size_t built = BuildFromMeshAsset(*c, nullptr, asset, manifest,
		"knight", 0, nullptr, false, nullptr,
		[](const std::string&) { return std::optional<std::vector<uint8_t>>{}; });
	Check(built == asset.nodes.size() - 1, "knight.build.nodecount");
	std::cout << "[knight] built " << built << " runtime nodes ("
		<< c->scene.hierarchy.size() << " with the scene root); skeleton "
		<< (c->hasSkeleton ? "yes" : "no") << ", root runtime-node "
		<< c->skeleton.root << "\n";
	// Units sanity: the Body blob's bind AABB + the root local's scale
	// (the glTF is centimeter-authored; the heap must read METERS).
	{
		const Scene& sc = c->scene;
		const auto& g = asset.geometry[0];
		std::cout << "[knight] Body blob aabb min=(" << g.aabbMin.x << ","
			<< g.aabbMin.y << "," << g.aabbMin.z << ") max=(" << g.aabbMax.x
			<< "," << g.aabbMax.y << "," << g.aabbMax.z << ") verts="
			<< g.positions.size() << "\n";
		const int rootNode = findNodeByName(sc, "knight_rig:root");
		const glm::mat4& L = sc.localTransform[size_t(rootNode)];
		std::cout << "[knight] root local: col0=(" << L[0].x << "," << L[0].y
			<< "," << L[0].z << ") col1=(" << L[1].x << "," << L[1].y << ","
			<< L[1].z << ") col2=(" << L[2].x << "," << L[2].y << ","
			<< L[2].z << ") t=(" << L[3].x << "," << L[3].y << "," << L[3].z
			<< ")\n";
	}

	const Scene& scene = c->scene;

	// ---- 2) The id contract: .anim -joint ids vs findNodeByName -------
	// The .anim records (from the export run): pelvis 8, spine_01 9,
	// hipSwinger 79, flaps 80..85, root 1.
	struct NamedId { const char* name; int expect; };
	const NamedId ids[] = {
		{ "knight_rig:root", 1 },
		{ "knight_rig:pelvis", 8 },
		{ "knight_rig:spine_01", 9 },
		{ "knight_rig:unrealhipSwinger_M", 79 },
		{ "knight_rig:unrealfrontFlap_01_R", 80 },
		{ "knight_rig:unrealbackFlap_02_R", 82 },
	};
	for (const NamedId& ni : ids) {
		const int found = findNodeByName(scene, ni.name);
		std::cout << "[knight] '" << ni.name << "' -> runtime node "
			<< found << " (anim says " << ni.expect << ")"
			<< " parent-chain: " << ParentChain(scene, found) << "\n";
		Check(found == ni.expect, ni.name);
	}
	// The hipSwinger chain must run through the pelvis.
	{
		const int hip = findNodeByName(scene, "knight_rig:unrealhipSwinger_M");
		const int pelvis = findNodeByName(scene, "knight_rig:pelvis");
		int cur = (hip >= 0) ? scene.hierarchy[size_t(hip)].parent : -1;
		bool viaPelvis = false;
		int guard = 0;
		while (cur >= 0 && guard++ < 64) {
			if (cur == pelvis) { viaPelvis = true; break; }
			cur = scene.hierarchy[size_t(cur)].parent;
		}
		Check(viaPelvis, "knight.hipchain.through-pelvis");
	}

	// ---- 3) The pose: attach the clip, sample two distant times -------
	smvk::touranim::AnimDoc doc;
	{
		std::ifstream f(anim, std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		std::vector<std::string> warns;
		if (!smvk::touranim::LoadTourAnimText(ss.str(), doc, err, &warns)) {
			std::cout << "FAIL LoadTourAnimText: " << err << "\n";
			return 1;
		}
		for (const std::string& w : warns)
			std::cout << "[knight] anim warn: " << w << "\n";
	}
	std::cout << "[knight] clip '" << doc.clips.front().name << "' "
		<< doc.clips.front().tracks.size() << " tracks, duration "
		<< doc.clips.front().duration << " s\n";
	// Bind locals snapshot (the root-track convention check compares
	// against the pre-animation local below).
	std::vector<glm::mat4> bindLocals = scene.localTransform;
	c->SetAnimClip(std::move(doc.clips.front()));

	// Joints of interest, resolved by name -> runtime id -> palette index
	// (palette[i] parallels skeleton.joints[i]).
	auto jointSlot = [&](const char* name) -> int {
		const int node = findNodeByName(scene, name);
		for (size_t i = 0; i < c->skeleton.joints.size(); ++i)
			if (int(c->skeleton.joints[i]) == node)
				return int(i);
		return -1;
	};
	struct Watch { const char* name; int slot; };
	std::vector<Watch> watch = {
		{ "knight_rig:pelvis", jointSlot("knight_rig:pelvis") },
		{ "knight_rig:spine_01", jointSlot("knight_rig:spine_01") },
		{ "knight_rig:thigh_l", jointSlot("knight_rig:thigh_l") },
		{ "knight_rig:unrealhipSwinger_M", jointSlot("knight_rig:unrealhipSwinger_M") },
		{ "knight_rig:unrealfrontFlap_01_R", jointSlot("knight_rig:unrealfrontFlap_01_R") },
		{ "knight_rig:unrealbackFlap_02_R", jointSlot("knight_rig:unrealbackFlap_02_R") },
	};
	for (const Watch& w : watch)
		std::cout << "[knight] watch '" << w.name << "' palette slot "
			<< w.slot << "\n";

	// Bind pose first: palette identity (slots' translations ~0) and the
	// pelvis->hipSwinger WORLD distance (the anatomical offset to hold).
	std::vector<glm::mat4> pal;
	float bindDist = -1.0f;
	{
		c->SampleAnimation(0.0f, pal); // t=0 (first keys)
		const int pelvisNode = findNodeByName(scene, "knight_rig:pelvis");
		const int hipNode = findNodeByName(scene, "knight_rig:unrealhipSwinger_M");
		bindDist = glm::distance(glm::vec3(scene.globalTransform[size_t(pelvisNode)][3]),
			glm::vec3(scene.globalTransform[size_t(hipNode)][3]));
		std::cout << "[knight] bind: pelvis<->hipSwinger world dist "
			<< bindDist << "\n";
		for (const Watch& w : watch)
			if (w.slot >= 0)
				std::cout << "[knight] bind palette[" << w.name << "] t=("
					<< pal[size_t(w.slot)][3].x << ", "
					<< pal[size_t(w.slot)][3].y << ", "
					<< pal[size_t(w.slot)][3].z << ")\n";
	}

	// Walk forward: 2 s then 6 s more (the pelvis translate track walks
	// the character; STEP-scale tracks and 2-key channels hold).
	c->SampleAnimation(2.0f, pal);
	const std::vector<glm::mat4> palA = pal;
	c->SampleAnimation(6.0f, pal);
	const std::vector<glm::mat4> palB = pal;
	// THE ROOT-TRACK CONVENTION CHECK: the runtime root local is the
	// coordFix-CONJUGATED glTF TRS (rotY(180) * authored); the .anim's
	// root rotate keys carry the RAW glTF quat. A track REPLACES the
	// channel - sampling the raw key SNAPS the root's yaw by the 180
	// basis flip, and every joint's palette entry inherits it as a
	// conjugated -180 rotation (the whole deformed body flips while
	// the mesh node's world - the draw frame - stays put).
	{
		const int rootNode = findNodeByName(scene, "knight_rig:root");
		const glm::mat4& L = scene.localTransform[size_t(rootNode)];
		// Yaw from the rotY basis: col0=(c,0,-s)*k, col2=(s,0,c)*k.
		const float yawNow = std::atan2(L[2].x, L[0].x) * 57.2957795f;
		const glm::mat4& Lb = bindLocals[size_t(rootNode)];
		const float yawBind = std::atan2(Lb[2].x, Lb[0].x) * 57.2957795f;
		std::cout << "[knight] root yaw: bind " << yawBind << "deg ->"
			" sampled " << yawNow << "deg (must hold: the .anim key is"
			" the coordFix-COMPOSED rotation the runtime local"
			" convention needs - raw glTF keys snapped it by -180)\n";
		Check(std::abs(yawNow - yawBind) < 0.5f, "knight.root.track-convention");
	}
	for (const Watch& w : watch) {
		if (w.slot < 0)
			continue;
		const glm::vec3 a(palA[size_t(w.slot)][3]);
		const glm::vec3 b(palB[size_t(w.slot)][3]);
		std::cout << "[knight] palette['" << w.name << "'] @2s (" << a.x
			<< ", " << a.y << ", " << a.z << ") @8s (" << b.x << ", "
			<< b.y << ", " << b.z << ") |delta| "
			<< glm::distance(a, b) << "\n";
	}

	// THE FOLLOW TEST: the pelvis moved (the walk is live), and the
	// hipSwinger's WORLD position must track the pelvis — the pelvis
	// <->hip distance stays the bind offset (the piece is attached),
	// NOT the bind-location distance (the piece stayed behind).
	{
		const int pelvisNode = findNodeByName(scene, "knight_rig:pelvis");
		const int hipNode = findNodeByName(scene, "knight_rig:unrealhipSwinger_M");
		const int flapNode = findNodeByName(scene, "knight_rig:unrealfrontFlap_01_R");
		const glm::vec3 pelvis(scene.globalTransform[size_t(pelvisNode)][3]);
		const glm::vec3 hip(scene.globalTransform[size_t(hipNode)][3]);
		const glm::vec3 flap(scene.globalTransform[size_t(flapNode)][3]);
		const float distHip = glm::distance(pelvis, hip);
		const float distFlap = glm::distance(pelvis, flap);
		std::cout << "[knight] @8s pelvis=(" << pelvis.x << ", " << pelvis.y
			<< ", " << pelvis.z << ") hip=(" << hip.x << ", " << hip.y
			<< ", " << hip.z << ") flap=(" << flap.x << ", " << flap.y
			<< ", " << flap.z << ")\n";
		std::cout << "[knight] @8s pelvis<->hip dist " << distHip
			<< " (bind " << bindDist << "), pelvis<->flap " << distFlap
			<< "\n";
		// The pelvis' own palette moved (the walk is live — otherwise the
		// comparison is vacuous).
		int pelvisSlot = jointSlot("knight_rig:pelvis");
		const float pelvisMoved = glm::distance(
			glm::vec3(palA[size_t(pelvisSlot)][3]),
			glm::vec3(palB[size_t(pelvisSlot)][3]));
		Check(pelvisMoved > 0.05f, "knight.walk.pelvis-moves");
		// Attached: the world offset pelvis<->hipSwinger stays within the
		// bind offset + the pelvis' own rotation sway (a generous 3x).
		Check(distHip < bindDist * 3.0f + 0.5f, "knight.hip.follows-pelvis");
	}

	// ---- 4) The skin stream: joint usage histogram over the Body mesh --
	// The tassets (hip plates) must ride the hip/flap joints (slots
	// 71..75). A vertex weighted to a bone OUTSIDE the pelvis chain
	// (e.g. an IK root under knight_rig:root) would swing in place at
	// the bind spot - "animated but doesn't follow".
	{
		// Slot -> joint node -> name table for readable dumps.
		std::vector<std::string> slotName(c->skeleton.joints.size());
		for (size_t i = 0; i < c->skeleton.joints.size(); ++i)
			slotName[i] = NodeName(scene, int(c->skeleton.joints[i]));
		const size_t bodyGeo = 0; // the 61040-vertex blob (mesh.0)
		if (bodyGeo < asset.geometry.size()
			&& !asset.geometry[bodyGeo].skin.empty()) {
			const auto& skin = asset.geometry[bodyGeo].skin;
			std::vector<uint32_t> use(c->skeleton.joints.size(), 0);
			std::vector<float> wsum(c->skeleton.joints.size(), 0.0f);
			for (const smvk::PackedSkinInfluences& s : skin) {
				const uint32_t j[4] = { s.joints & 0xFFu,
					(s.joints >> 8) & 0xFFu,
					(s.joints >> 16) & 0xFFu,
					(s.joints >> 24) & 0xFFu };
				const float w[4] = {
					float(s.weights & 0xFF) / 255.0f,
					float((s.weights >> 8) & 0xFF) / 255.0f,
					float((s.weights >> 16) & 0xFF) / 255.0f,
					float((s.weights >> 24) & 0xFF) / 255.0f };
				for (int k = 0; k < 4; ++k) {
					if (j[k] < use.size() && w[k] > 0.0f) {
						++use[j[k]];
						wsum[j[k]] += w[k];
					}
				}
			}
			std::cout << "[knight] Body skin usage (slot, name, verts,"
				" weight-sum):\n";
			for (size_t i = 0; i < use.size(); ++i)
				if (use[i] > 0)
					std::cout << "  slot " << i << " " << slotName[i]
						<< " verts=" << use[i] << " wsum=" << wsum[i]
						<< "\n";
			// The hip flap joints must own vertices.
			const int hipSlots[] = { 71, 72, 73, 74, 75 };
			bool flapsWeighted = false;
			for (int s : hipSlots)
				if (s < int(use.size()) && use[size_t(s)] > 0)
					flapsWeighted = true;
			Check(flapsWeighted, "knight.hip.flap-joints-weighted");
		}
	}

	std::cout << g_checks - g_failures << "/" << g_checks
		<< " checks passed\n";
	std::cout << (g_failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
	return g_failures == 0 ? 0 : 1;
}
