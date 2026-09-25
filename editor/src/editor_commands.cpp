// Editor CRUD command stack: execute/do/undo/redo + .tour persistence.
//
// Live commands use WORLD-space ids; the .tour file uses CLUSTER-space ids
// (import-deterministic). Translation happens only at the save/load
// boundary (CmdToTour / TourToCmd).

#include "editor_app.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <Engine/Asset/tour.h>
#include <Engine/Flintbase/flintbase_backend.h>
#include <Engine/Scene/scene.h>
#include <Engine/Utils/texture_loader.h>
#include <Engine/Vfs/vfs.h>

namespace editor {

void EditorApp::ReadLocalTRS(uint32_t node, glm::vec3& t,
	glm::vec3& rDeg, glm::vec3& s) const {
	const glm::mat4& m = m_world->scene.localTransform[node];
	t = glm::vec3(m[3]);
	glm::vec3 skew;
	glm::vec4 persp;
	glm::quat q;
	glm::decompose(m, s, q, t, skew, persp);
	rDeg = glm::degrees(glm::eulerAngles(q));
}

void EditorApp::WriteLocalTRS(uint32_t node, const glm::vec3& t,
	const glm::vec3& rDeg, const glm::vec3& s) {
	m_world->scene.localTransform[node] = LocalMatrixFromTRS(t, rDeg, s);
	markAsChanged(m_world->scene, int(node));
	recalculateGlobalTransforms(m_world->scene);
}

// Current shared-material attr value (matches tour ApplyMatAttr fields).
void EditorApp::ReadSharedMatAttr(uint32_t matWorld, const std::string& attr,
	glm::vec4& v4, double& num) {
	v4 = glm::vec4(0.0f);
	num = 0.0;
	smvk::Material* mat = m_world->GetMaterial(matWorld);
	if (!mat)
		return;
	if (attr == "baseColor")
		v4 = mat->values.diffuse;
	else if (attr == "emissive")
		v4 = glm::vec4(mat->values.emissiveFactor.x,
			mat->values.emissiveFactor.y, mat->values.emissiveFactor.z,
			mat->values.params2.z);
	else if (attr == "metallic")
		num = mat->values.params.y;
	else if (attr == "roughness")
		num = mat->values.params.z;
	else if (attr == "alphaCutoff")
		num = mat->values.params.x;
	else if (attr == "emissiveStrength")
		num = mat->values.params2.z;
	else if (attr == "normalScale")
		num = mat->values.params2.y;
	else if (attr == "occlusionStrength")
		num = mat->values.params2.x;
	else if (attr == "doubleSided")
		num = mat->doubleSided ? 1.0 : 0.0;
	else if (attr == "castsShadows")
		num = mat->castsShadows ? 1.0 : 0.0;
	else if (attr == "alphaMode")
		num = double(int(mat->alphaMode));
	else if (attr == "layers") {
		// Vertex-color layered materials (see mesh.h): x/y/z = WORLD
		// material ids of layers 1..3 (-1 none, live-world domain),
		// w = active layer count (0 = legacy tint). The tour boundary
		// translates world ids <-> cluster pool ids.
		v4 = glm::vec4(float(mat->values.layerMaterials[0]),
			float(mat->values.layerMaterials[1]),
			float(mat->values.layerMaterials[2]),
			float(mat->values.layerMaterials.w));
		num = double(mat->values.layerMaterials.w);
	}
	// Texture slots: RHI pool indices (-1 none) — the same domain both
	// sides of the tour boundary (materials store pool indices).
	else if (attr == "baseColorTex")
		num = double(mat->values.textures.x);
	else if (attr == "metallicRoughTex")
		num = double(mat->values.textures.y);
	else if (attr == "normalTex")
		num = double(mat->values.textures.z);
	else if (attr == "occlusionTex")
		num = double(mat->values.textures.w);
	else if (attr == "emissiveTex")
		num = double(mat->values.textures2.x);
}

void EditorApp::WriteSharedMatAttr(uint32_t matWorld, const std::string& attr,
	const glm::vec4& v4, double num) {
	smvk::Material* mat = m_world->GetMaterial(matWorld);
	if (!mat)
		return;
	if (attr == "baseColor")
		mat->values.diffuse = v4;
	else if (attr == "emissive") {
		mat->values.emissiveFactor = glm::vec4(v4.x, v4.y, v4.z, 0.0f);
		mat->values.params2.z = v4.w;
	}
	else if (attr == "metallic")
		mat->values.params.y = float(num);
	else if (attr == "roughness")
		mat->values.params.z = float(num);
	else if (attr == "alphaCutoff")
		mat->values.params.x = float(num);
	else if (attr == "emissiveStrength")
		mat->values.params2.z = float(num);
	else if (attr == "normalScale")
		mat->values.params2.y = float(num);
	else if (attr == "occlusionStrength")
		mat->values.params2.x = float(num);
	else if (attr == "doubleSided")
		mat->doubleSided = (num != 0.0);
	else if (attr == "castsShadows")
		mat->castsShadows = (num != 0.0);
	else if (attr == "alphaMode") {
		mat->alphaMode = (num < 0.5) ? smvk::AlphaMode::Opaque
			: (num < 1.5) ? smvk::AlphaMode::Mask : smvk::AlphaMode::Blend;
		mat->shadingModel = (mat->alphaMode == smvk::AlphaMode::Mask)
			? smvk::ShadingModel::Masked : smvk::ShadingModel::DefaultLit;
		mat->values.params.w =
			(mat->alphaMode == smvk::AlphaMode::Mask) ? 2.0f : 0.0f;
		mat->values.params2.w = float(int(mat->alphaMode));
	}
	else if (attr == "layers") {
		// v4 in the LIVE-WORLD domain (see ReadSharedMatAttr).
		mat->values.layerMaterials = glm::ivec4(
			int(v4.x), int(v4.y), int(v4.z), int(v4.w));
	}
	// Texture slots: pool indices, -1 clears (see ReadSharedMatAttr).
	else if (attr == "baseColorTex")
		mat->values.textures.x = int(num);
	else if (attr == "metallicRoughTex")
		mat->values.textures.y = int(num);
	else if (attr == "normalTex")
		mat->values.textures.z = int(num);
	else if (attr == "occlusionTex")
		mat->values.textures.w = int(num);
	else if (attr == "emissiveTex")
		mat->values.textures2.x = int(num);
	mat->rebuildPipeline();
	RefreshMaterialNodes(matWorld);
}

glm::mat4 EditorApp::LocalMatrixFromTRS(const glm::vec3& t,
	const glm::vec3& eulerDeg, const glm::vec3& s) {
	glm::mat4 m(1.0f);
	m = glm::translate(m, t);
	m = glm::rotate(m, glm::radians(eulerDeg.z), glm::vec3(0, 0, 1));
	m = glm::rotate(m, glm::radians(eulerDeg.y), glm::vec3(0, 1, 0));
	m = glm::rotate(m, glm::radians(eulerDeg.x), glm::vec3(1, 0, 0));
	m = glm::scale(m, s);
	return m;
}

int EditorApp::WorldToCluster(uint32_t worldNode) const {
	if (!m_world || m_clusterBase == UINT32_MAX || m_world->clusters.empty())
		return -1;
	if (worldNode < m_clusterBase)
		return -1;
	const uint32_t cn = worldNode - m_clusterBase;
	if (cn >= m_world->clusters[0]->scene.hierarchy.size())
		return -1;
	return int(cn);
}

uint32_t EditorApp::ClusterToWorld(uint32_t clusterNode) const {
	if (m_clusterBase == UINT32_MAX)
		return UINT32_MAX;
	return m_clusterBase + clusterNode;
}

int EditorApp::WorldMatToCluster(uint32_t worldMat) const {
	if (!m_world || worldMat >= m_world->materialLocations.size())
		return -1;
	const auto& loc = m_world->materialLocations[worldMat];
	if (!loc.cluster || !loc.handle.valid())
		return -1;
	return int(loc.handle.index()); // cluster pool index (deterministic)
}

int EditorApp::ClusterMatToWorld(uint32_t poolIdx) const {
	if (!m_world)
		return -1;
	for (uint32_t i = 0; i < uint32_t(m_world->materialLocations.size()); ++i) {
		const auto& loc = m_world->materialLocations[i];
		if (loc.cluster && loc.handle.valid() && loc.handle.index() == poolIdx)
			return int(i);
	}
	return -1;
}

void EditorApp::ExecCmd(EditCmd& c, bool forward) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	switch (c.verb) {
	case EditVerb::Rename: {
		const std::string& name = forward ? c.afterText : c.beforeText;
		const uint32_t id = uint32_t(scene.nodeNames.size());
		scene.nodeNames.push_back(name);
		scene.nameForNode[c.node] = id;
		break;
	}
	case EditVerb::Translate:
	case EditVerb::Rotate:
	case EditVerb::Scale: {
		if (forward) {
			glm::vec3 t, r, s;
			ReadLocalTRS(c.node, t, r, s);
			if (c.verb == EditVerb::Translate)
				t = c.afterVec;
			else if (c.verb == EditVerb::Rotate)
				r = c.afterVec;
			else
				s = c.afterVec;
			scene.localTransform[c.node] = LocalMatrixFromTRS(t, r, s);
		}
		else {
			scene.localTransform[c.node] = c.beforeMat;
		}
		markAsChanged(scene, int(c.node));
		recalculateGlobalTransforms(scene);
		break;
	}
	case EditVerb::AssignMaterial: {
		scene.materialForNode[c.node] = forward ? c.material : c.beforeMaterial;
		RefreshNodeDrawState(c.node);
		break;
	}
	case EditVerb::SetMatAttr: {
		if (forward)
			WriteSharedMatAttr(c.matWorld, c.attr, c.vec4, c.number);
		else
			WriteSharedMatAttr(c.matWorld, c.attr, c.beforeVec4, c.beforeNum);
		break;
	}
	case EditVerb::SetNoLightmap: {
		const bool v = forward ? c.afterFlag : c.beforeFlag;
		if (v)
			MarkNoLightmap(scene, int(c.node));
		else
			ClearNoLightmap(scene, int(c.node));
		break;
	}
	case EditVerb::SetDitherCloseup: {
		const bool v = forward ? c.afterFlag : c.beforeFlag;
		if (v)
			MarkDitherCloseup(scene, int(c.node));
		else
			ClearDitherCloseup(scene, int(c.node));
		break;
	}
	case EditVerb::SetVisibility: {
		const bool hidden = forward ? c.afterFlag : c.beforeFlag;
		if (hidden)
			MarkHidden(scene, int(c.node));
		else
			ClearHidden(scene, int(c.node));
		break;
	}
	case EditVerb::Parent: {
		ReparentNode(c.node, forward ? c.node2 : c.beforeParent);
		break;
	}
	case EditVerb::CreateLight: {
		if (forward)
			ExecCreateLight(c);
		else
			ExecUncreateLight(c);
		break;
	}
	case EditVerb::CreateTerrain: {
		if (forward)
			ExecCreateTerrain(c);
		else
			ExecUncreateTerrain(c);
		break;
	}
	case EditVerb::SetLightAttr: {
		const std::string& attr = c.lightAttr;
		if (forward)
			WriteNodeLightAttr(c.node, attr, c.vec4, c.number,
				c.afterFlag, c.afterText);
		else
			WriteNodeLightAttr(c.node, attr, c.beforeVec4, c.beforeNum,
				c.beforeFlag, c.beforeText);
		break;
	}
	case EditVerb::TerrainPaint: {
		if (forward)
			ExecTerrainPaint(c);
		else
			ExecUnterrainPaint(c);
		break;
	}
	case EditVerb::Delete: {
		if (forward)
			ExecDelete(c);
		else
			ExecUndelete(c);
		break;
	}
	case EditVerb::CreateTransform: {
		if (forward)
			ExecCreateTransform(c);
		else
			ExecUncreateTransform(c);
		break;
	}
	case EditVerb::CreateCVolume: {
		if (forward)
			ExecCreateCVolume(c);
		else
			ExecUncreateCvolume(c);
		break;
	}
	case EditVerb::SetCvolAttr: {
		const std::string& attr = c.cvolAttr;
		if (forward)
			WriteNodeCvolAttr(c.node, attr, c.vec4, c.number,
				c.afterText);
		else
			WriteNodeCvolAttr(c.node, attr, c.beforeVec4, c.beforeNum,
				c.beforeText);
		break;
	}
	}
}

// ---------------------------------------------------------------------------
// Created lights (point/spot).
//
// A created light is a WORLD node appended to the live scene plus a
// SceneLight definition in the primary cluster's lights pool, linked
// through world lightLocations — so gizmos (RecordGizmoPass), the
// inspector (FindNodeLight), the outliner tags and the cooked-lights
// census all pick it up with no special cases. Undo removes the node
// when it is still the childless tail (ids stay stable); otherwise the
// light is tombstoned (hidden + detached) for the same reason.
// Tour persistence: CreateLight carries the creation (CmdToTour emits
// the LIVE params, so post-creation edits ride along); SetLightAttr
// addresses later creates via kCreateLightRefBase (creation order).
// ---------------------------------------------------------------------------
uint32_t EditorApp::DefaultCreateParent() const {
	if (!m_world || m_world->scene.hierarchy.empty())
		return UINT32_MAX;
	// The primary cluster's root: WorldToCluster maps it to cluster id
	// 0, so the tour stays replayable on a fresh import.
	if (m_clusterBase != UINT32_MAX
		&& m_clusterBase < m_world->scene.hierarchy.size())
		return m_clusterBase;
	return 0;
}

std::string EditorApp::UniqueLightName(bool spot) const {
	const char* base = spot ? "Spot" : "Point";
	int n = 1;
	if (m_world)
		for (const auto& [node, li] : m_world->scene.lightForNode) {
			const ::SceneLight* L = FindNodeLight(node, nullptr);
			if (L && (L->type == ::SceneLight::Type::Spot) == spot)
				++n;
		}
	return std::string(base) + "_" + std::to_string(n);
}

int EditorApp::CreatedLightIndex(uint32_t worldNode) const {
	// Creation-order index among history creates at/below the cursor:
	// the -ref value CmdToTour emits for SetLightAttr on created nodes.
	// Deleted (detached) creates are skipped: their CreateLight is
	// dropped from the tour (live-state emission), so the file's -ref
	// space only spans ALIVE creates — this keeps save-time indices and
	// replay-time m_tourCreatedNodes addressing identical.
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateLight)
			continue;
		if (!m_world || !m_world->scene.lightForNode.contains(h.node))
			continue; // deleted (tombstoned): not in the file's ref space
		if (h.node == worldNode)
			return count;
		++count;
	}
	return -1;
}

int EditorApp::CreatedCvolIndex(uint32_t worldNode) const {
	// Same contract as CreatedLightIndex, for the cvolume family:
	// alive creates only (a deleted cvolume's CreateCVolume is dropped
	// from the tour, so the file's -ref space spans the survivors —
	// save-time indices and replay-time m_tourCreatedCvolNodes match).
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateCVolume)
			continue;
		if (!m_world || !m_world->scene.cvolForNode.contains(h.node))
			continue; // deleted (tombstoned): not in the file's ref space
		if (h.node == worldNode)
			return count;
		++count;
	}
	return -1;
}

std::string EditorApp::UniqueCvolName() const {
	// Next free "CVol_N" over live node names (1-based).
	int n = 1;
	if (m_world) {
		for (;;) {
			const std::string want = "CVol_" + std::to_string(n);
			bool taken = false;
			for (size_t i = 0; i < m_world->scene.hierarchy.size() && !taken; ++i)
				taken = getNodeName(m_world->scene, int(i)) == want;
			if (!taken)
				break;
			++n;
		}
	}
	return "CVol_" + std::to_string(n);
}

std::string EditorApp::UniqueTerrainName() const {
	// Next free "Terrain_N" over live node names (1-based).
	int n = 1;
	if (m_world) {
		for (;;) {
			const std::string want = "Terrain_" + std::to_string(n);
			bool taken = false;
			for (size_t i = 0; i < m_world->scene.hierarchy.size() && !taken; ++i)
				taken = getNodeName(m_world->scene, int(i)) == want;
			if (!taken)
				break;
			++n;
		}
	}
	return "Terrain_" + std::to_string(n);
}

int EditorApp::CreatedTerrainIndex(uint32_t worldNode) const {
	// Same contract as CreatedLightIndex: alive creates only (deleted
	// groups' CreateTerrain is dropped from the tour, so the file's
	// TerrainPaint -ref space spans the survivors).
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateTerrain)
			continue;
		auto it = m_terrainGroups.find(h.node);
		if (it == m_terrainGroups.end() || it->second.deleted)
			continue; // deleted (tombstoned): not in the file's ref space
		if (h.node == worldNode)
			return count;
		++count;
	}
	return -1;
}

void EditorApp::DraftCreateTerrain(const TerrainDraft& d) {
	if (!m_world) {
		Log("Create terrain: no scene open.");
		return;
	}
	EditCmd c{};
	c.verb = EditVerb::CreateTerrain;
	c.node = UINT32_MAX; // assigned at forward execution
	c.node2 = (d.tilesX & 0xFFFFu) | ((d.tilesZ & 0xFFFFu) << 16);
	// Parent rides beforeParent (node2 is the tile split now).
	c.beforeParent = (m_selected != UINT32_MAX
		&& m_selected < m_world->scene.hierarchy.size())
		? m_selected : DefaultCreateParent();
	c.afterText = d.name;
	c.afterVec = d.origin;
	c.vec4 = glm::vec4(d.size, float(d.res), d.height, d.freq);
	c.number = double(d.seed);
	c.material = d.material;
	Do(c);
}

void EditorApp::DraftCreateLight(bool spot) {
	if (!m_world) {
		Log("Create light: no scene open.");
		return;
	}
	EditCmd c{};
	c.verb = EditVerb::CreateLight;
	c.node = UINT32_MAX; // assigned at forward execution
	const uint32_t sel = m_selected;
	c.node2 = (sel != UINT32_MAX && sel < m_world->scene.hierarchy.size())
		? sel : DefaultCreateParent();
	c.afterText = UniqueLightName(spot);
	c.afterVec = ActiveCam().target;
	c.lightType = spot ? 2u : 1u;
	c.lightColor = glm::vec3(1.0f, 0.95f, 0.85f);
	c.lightIntensity = spot ? 1500.0f : 800.0f;
	c.lightRange = 25.0f;
	c.lightInnerDeg = 20.0f;
	c.lightOuterDeg = 35.0f;
	c.lightStatic = false; // dynamic: visible in the viewport immediately
	c.lightShadow = spot;  // spots default to shadow-casting (points can't)
	Do(c);
}

void EditorApp::ExecCreateLight(EditCmd& c) {
	if (!m_world || m_world->clusters.empty()
		|| !m_world->clusters[0])
		return;
	Scene& scene = m_world->scene;
	uint32_t parent = c.node2;
	if (parent == UINT32_MAX || parent >= scene.hierarchy.size())
		parent = DefaultCreateParent();
	if (parent != UINT32_MAX && parent >= scene.hierarchy.size())
		return;
	const int level = (parent == UINT32_MAX) ? 0
		: scene.hierarchy[parent].level + 1;
	const int created = addNode(scene,
		parent == UINT32_MAX ? -1 : int(parent), level);
	if (created < 0)
		return;
	c.node = uint32_t(created);
	scene.localTransform[size_t(created)] =
		glm::translate(glm::mat4(1.0f), c.afterVec);
	// Name (the nodeNames pool is grow-only, like Rename).
	const uint32_t nameId = uint32_t(scene.nodeNames.size());
	scene.nodeNames.push_back(c.afterText);
	scene.nameForNode[uint32_t(created)] = nameId;
	// Pool definition on the primary cluster (FindNodeLight resolves
	// world lights through lightLocations -> cluster + handle).
	::SceneLight def{};
	def.type = c.lightType == 2u
		? ::SceneLight::Type::Spot : ::SceneLight::Type::Point;
	def.color = c.lightColor;
	def.intensity = std::max(c.lightIntensity, 0.0f);
	def.range = std::max(c.lightRange, 0.0f);
	def.innerConeAngle = glm::radians(std::clamp(c.lightInnerDeg, 0.0f, 89.0f));
	def.outerConeAngle = glm::radians(std::clamp(c.lightOuterDeg, 0.0f, 89.0f));
	if (c.lightStatic)
		def.flags |= ::SceneLight::kSceneLightFlag_Static;
	if (c.lightShadow && def.type == ::SceneLight::Type::Spot)
		def.flags |= ::SceneLight::kSceneLightFlag_CastShadow;
	def.name = c.afterText;
	smvk::Cluster* cl = m_world->clusters[0].get();
	auto handle = cl->lights.create(std::move(def));
	const uint32_t worldLightIdx = uint32_t(m_world->lightLocations.size());
	m_world->lightLocations.push_back({ cl, handle });
	scene.lightForNode[uint32_t(created)] = worldLightIdx;
	if (m_replayingTour)
		m_tourCreatedNodes.push_back(uint32_t(created));
	markAsChanged(scene, created);
	recalculateGlobalTransforms(scene);
	m_selected = uint32_t(created);
	char buf[160];
	std::snprintf(buf, sizeof(buf), "Created %s light '%s' (#%u)",
		c.lightType == 2u ? "spot" : "point", c.afterText.c_str(),
		uint32_t(created));
	Log(buf);
}

void EditorApp::ExecUncreateLight(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	const bool tail = (n + 1 == uint32_t(scene.hierarchy.size()));
	const bool childless = scene.hierarchy[n].firstChild < 0;
	auto lit = scene.lightForNode.find(n);
	const bool lightTail = lit != scene.lightForNode.end()
		&& lit->second + 1 == m_world->lightLocations.size();
	if (tail && childless) {
		// Physical removal: pop every vector addNode grew (flag arrays
		// only when they are exactly node-sized — empty/mismatched
		// means "default", which needs no entry).
		if (lit != scene.lightForNode.end()) {
			if (lightTail) {
				const auto& loc = m_world->lightLocations[lit->second];
				if (loc.cluster)
					loc.cluster->lights.destroy(loc.handle);
				m_world->lightLocations.pop_back();
			}
			scene.lightForNode.erase(n);
		}
		scene.nameForNode.erase(n);
		const int p = scene.hierarchy[n].parent;
		if (p >= 0 && size_t(p) < scene.hierarchy.size()) {
			auto& hp = scene.hierarchy[size_t(p)];
			if (hp.firstChild == int(n)) {
				hp.firstChild = scene.hierarchy[n].nextSibling;
			} else {
				int prev = hp.firstChild;
				while (prev >= 0
					&& scene.hierarchy[size_t(prev)].nextSibling != int(n))
					prev = scene.hierarchy[size_t(prev)].nextSibling;
				if (prev >= 0)
					scene.hierarchy[size_t(prev)].nextSibling =
						scene.hierarchy[n].nextSibling;
			}
			// Refresh the cached tail on the (possibly new) first child.
			int tailId = -1;
			for (int s = hp.firstChild; s >= 0;
				s = scene.hierarchy[size_t(s)].nextSibling)
				tailId = s;
			if (hp.firstChild >= 0)
				scene.hierarchy[size_t(hp.firstChild)].lastSibling = tailId;
		}
		scene.localTransform.pop_back();
		scene.globalTransform.pop_back();
		scene.hierarchy.pop_back();
		if (scene.staticRoot.size() == n + 1)
			scene.staticRoot.pop_back();
		if (scene.staticNode.size() == n + 1)
			scene.staticNode.pop_back();
		if (scene.noLightmap.size() == n + 1)
			scene.noLightmap.pop_back();
		if (scene.ditherCloseup.size() == n + 1)
			scene.ditherCloseup.pop_back();
		if (scene.hidden.size() == n + 1)
			scene.hidden.pop_back();
		recalculateGlobalTransforms(scene);
		if (m_selected == n)
			m_selected = (p >= 0) ? uint32_t(p) : UINT32_MAX;
		Log("Undid light creation (#" + std::to_string(n) + ").");
	} else {
		// Non-tail (or parented) node: keep ids stable — hide the node
		// and detach the light instead of shifting the world.
		MarkHidden(scene, int(n));
		scene.lightForNode.erase(n);
		Log("Undid light creation (#" + std::to_string(n) +
		 "): hidden, ids kept stable (pool entry retained).");
	}
}

// ---------------------------------------------------------------------------
// Created convex volumes (cvolume nodes).
//
// A created cvolume is a WORLD transform node (translate = box center,
// scale = full extents, axis-aligned — rotation ignored by design) plus
// a node-local SceneCvol definition in scene.cvolForNode — so gizmos,
// the inspector, the outliner and the tour all pick it up with no
// special cases. Undo removes the node when it is still the childless
// tail (ids stay stable); otherwise the node is tombstoned (hidden +
// def detached) for the same reason.
// Tour persistence: CreateCVolume carries the creation (CmdToTour emits
// the LIVE params, so post-creation edits ride along); SetCvolAttr
// addresses later creates via kCreateCvolRefBase (creation order).
// ---------------------------------------------------------------------------
void EditorApp::DraftCreateCVolume() {
	if (!m_world) {
		Log("Create cvolume: no scene open.");
		return;
	}
	EditCmd c{};
	c.verb = EditVerb::CreateCVolume;
	c.node = UINT32_MAX; // assigned at forward execution
	const uint32_t sel = m_selected;
	c.node2 = (sel != UINT32_MAX && sel < m_world->scene.hierarchy.size())
		? sel : DefaultCreateParent();
	c.afterText = UniqueCvolName();
	c.afterVec = ActiveCam().target;
	c.svec = glm::vec3(8.0f); // 8 m authoring box
	c.cvolPriority = 0;
	c.cvolSpacing = 0.5f;
	c.cvolInsideFade = 0.1f;
	c.cvolOutsideFade = 0.1f;
	Do(c);
}

void EditorApp::ExecCreateCVolume(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	uint32_t parent = c.node2;
	if (parent == UINT32_MAX || parent >= scene.hierarchy.size())
		parent = DefaultCreateParent();
	if (parent != UINT32_MAX && parent >= scene.hierarchy.size())
		return;
	const int level = (parent == UINT32_MAX) ? 0
		: scene.hierarchy[parent].level + 1;
	const int created = addNode(scene,
		parent == UINT32_MAX ? -1 : int(parent), level);
	if (created < 0)
		return;
	c.node = uint32_t(created);
	// TRS = translate + scale-as-size (axis-aligned by design; any
	// rotation the command carries is dropped — the volume shape
	// never reads it).
	scene.localTransform[size_t(created)] =
		glm::translate(glm::mat4(1.0f), c.afterVec)
		* glm::scale(glm::mat4(1.0f), glm::vec3(
			std::max(c.svec.x, 1e-3f), std::max(c.svec.y, 1e-3f),
			std::max(c.svec.z, 1e-3f)));
	const uint32_t nameId = uint32_t(scene.nodeNames.size());
	scene.nodeNames.push_back(c.afterText);
	scene.nameForNode[uint32_t(created)] = nameId;
	::SceneCvol def{};
	def.priority = c.cvolPriority;
	def.spacing = std::max(c.cvolSpacing, 1e-3f);
	def.insideFade = std::max(c.cvolInsideFade, 0.0f);
	def.outsideFade = std::max(c.cvolOutsideFade, 0.0f);
	scene.cvolForNode[uint32_t(created)] = def;
	if (m_replayingTour)
		m_tourCreatedCvolNodes.push_back(uint32_t(created));
	markAsChanged(scene, created);
	recalculateGlobalTransforms(scene);
	m_selected = uint32_t(created);
	char buf[160];
	std::snprintf(buf, sizeof(buf), "Created cvolume '%s' (#%u)",
		c.afterText.c_str(), uint32_t(created));
	Log(buf);
}

void EditorApp::ExecUncreateCvolume(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	const bool tail = (n + 1 == uint32_t(scene.hierarchy.size()));
	const bool childless = scene.hierarchy[n].firstChild < 0;
	if (tail && childless) {
		// Physical removal: pop every vector addNode grew (flag arrays
		// only when they are exactly node-sized — empty/mismatched
		// means "default", which needs no entry).
		scene.cvolForNode.erase(n);
		scene.nameForNode.erase(n);
		const int p = scene.hierarchy[n].parent;
		if (p >= 0 && size_t(p) < scene.hierarchy.size()) {
			auto& hp = scene.hierarchy[size_t(p)];
			if (hp.firstChild == int(n)) {
				hp.firstChild = scene.hierarchy[n].nextSibling;
			} else {
				int prev = hp.firstChild;
				while (prev >= 0
					&& scene.hierarchy[size_t(prev)].nextSibling != int(n))
					prev = scene.hierarchy[size_t(prev)].nextSibling;
				if (prev >= 0)
					scene.hierarchy[size_t(prev)].nextSibling =
						scene.hierarchy[n].nextSibling;
			}
			int tailId = -1;
			for (int s = hp.firstChild; s >= 0;
				s = scene.hierarchy[size_t(s)].nextSibling)
				tailId = s;
			if (hp.firstChild >= 0)
				scene.hierarchy[size_t(hp.firstChild)].lastSibling = tailId;
		}
		scene.localTransform.pop_back();
		scene.globalTransform.pop_back();
		scene.hierarchy.pop_back();
		if (scene.staticRoot.size() == n + 1)
			scene.staticRoot.pop_back();
		if (scene.staticNode.size() == n + 1)
			scene.staticNode.pop_back();
		if (scene.noLightmap.size() == n + 1)
			scene.noLightmap.pop_back();
		if (scene.ditherCloseup.size() == n + 1)
			scene.ditherCloseup.pop_back();
		if (scene.hidden.size() == n + 1)
			scene.hidden.pop_back();
		recalculateGlobalTransforms(scene);
		if (m_selected == n)
			m_selected = (p >= 0) ? uint32_t(p) : UINT32_MAX;
		Log("Undid cvolume creation (#" + std::to_string(n) + ").");
	} else {
		// Non-tail (or parented) node: keep ids stable — hide the node
		// and detach the def instead of shifting the world.
		MarkHidden(scene, int(n));
		scene.cvolForNode.erase(n);
		Log("Undid cvolume creation (#" + std::to_string(n) +
			"): hidden, ids kept stable (def retained in the command).");
	}
}

// ---------------------------------------------------------------------------
// Delete (Del key / Edit menu, Maya delete): remove a node from the LIVE
// scene. Links drop — meshForNode/materialForNode (draw state), lightForNode
// (light census/gizmos/cook), drawItems (draws/picks/box-select) — and the
// node id joins m_deletedNodes so the outliner filters it. NO hiding, NO
// renumbering: ids stay stable (later commands still address the node) and
// pool resources are never reclaimed, only the links. A terrain GROUP
// deletes as a unit (group + every tile). Undo restores the exact prior
// state from the captured links/entries. Tour: cluster-backed nodes persist
// as Verb::Delete (delete -n <id>), replayed by the editor AND PakCooker;
// editor-created nodes (no cluster id) instead vanish from the file through
// the creates' live-state emission.
// ---------------------------------------------------------------------------
void EditorApp::ExecDelete(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n == 0 || n >= scene.hierarchy.size())
		return; // 0 = scene root: never a delete target
	if (m_deletedNodes.contains(n))
		return; // already deleted (idempotent replay)
	// Delete set: group + tiles as one unit; everything else per-node.
	std::vector<uint32_t> nodes{ n };
	auto git = m_terrainGroups.find(n);
	if (git != m_terrainGroups.end()) {
		git->second.deleted = true;
		nodes.insert(nodes.end(), git->second.tileNodes.begin(),
			git->second.tileNodes.end());
	}
	// Capture the exact prior state ONCE (first forward exec; redos
	// reuse — LIFO undo guarantees the live vectors are back to this
	// exact state whenever the capture is needed again).
	if (c.delNodeMaps.empty()) {
		auto captureMaps = [&](uint32_t id) {
			uint32_t hasMesh = 0, mesh = 0, hasMat = 0, mat = 0;
			auto m = scene.meshForNode.find(id);
			if (m != scene.meshForNode.end()) {
				hasMesh = 1;
				mesh = m->second;
			}
			auto a = scene.materialForNode.find(id);
			if (a != scene.materialForNode.end()) {
				hasMat = 1;
				mat = a->second;
			}
			c.delNodeMaps.insert(c.delNodeMaps.end(),
				{ id, hasMesh, mesh, hasMat, mat });
		};
		captureMaps(n);
		if (git != m_terrainGroups.end())
			for (uint32_t t : git->second.tileNodes)
				captureMaps(t);
		// Light link (created or imported): the world index rides the
		// command (-1 = none); the pool entry itself stays alive.
		auto lit = scene.lightForNode.find(n);
		c.number = (lit != scene.lightForNode.end())
			? double(lit->second) : -1.0;
		// Cvolume def (created or replayed): node-local floats ride
		// beforeVec4 (priority, spacing, inside, outside) + presence
		// rides beforeFlag — the light-index precedent for the exact
		// undo below.
		auto cvit = scene.cvolForNode.find(n);
		c.beforeFlag = (cvit != scene.cvolForNode.end());
		if (c.beforeFlag)
			c.beforeVec4 = glm::vec4(float(cvit->second.priority),
				cvit->second.spacing, cvit->second.insideFade,
				cvit->second.outsideFade);
		// Draw entries (position, node, mesh, material, flags).
		for (uint32_t id : nodes)
			for (size_t i = 0; i < m_world->drawItems.size(); ++i)
				if (m_world->drawItems[i].node == id) {
					const smvk::World::RenderItem& item =
						m_world->drawItems[i];
					c.delDrawItems.insert(c.delDrawItems.end(),
						{ uint32_t(i), item.node, item.mesh,
						  item.material, item.flags });
				}
	}
	// Unlink: maps, light, draw entries; the ids join the deleted set.
	auto dropSelected = [&](uint32_t id) {
		for (size_t i = m_selection.size(); i-- > 0;)
			if (m_selection[i] == id)
				m_selection.erase(m_selection.begin() + ptrdiff_t(i));
		if (m_selected == id)
			m_selected = m_selection.empty()
				? UINT32_MAX : m_selection.back();
	};
	for (uint32_t id : nodes) {
		scene.meshForNode.erase(id);
		scene.materialForNode.erase(id);
		scene.cvolForNode.erase(id);
		m_deletedNodes.insert(id);
		for (size_t i = m_world->drawItems.size(); i-- > 0;)
			if (m_world->drawItems[i].node == id) {
				m_world->drawItems.erase(m_world->drawItems.begin()
					+ ptrdiff_t(i));
				m_world->itemFlags.erase(m_world->itemFlags.begin()
					+ ptrdiff_t(i));
			}
		dropSelected(id);
	}
	scene.lightForNode.erase(n);
	m_world->flagLists.clear();
	Log("Deleted '" + getNodeName(scene, int(n)) + "' (#" +
		std::to_string(n) + ") — Ctrl+Z restores it.");
}

void EditorApp::ExecUndelete(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	// Restore the captured links (order-free): mesh/material maps and
	// deleted-set membership per node, the light link, then the draw
	// entries at their ORIGINAL positions in ascending order (every
	// earlier reinsert already restored its shift, so each index lands
	// exactly where it was).
	for (size_t k = 0; k + 4 < c.delNodeMaps.size(); k += 5) {
		const uint32_t id = c.delNodeMaps[k];
		if (c.delNodeMaps[k + 1])
			scene.meshForNode[id] = c.delNodeMaps[k + 2];
		if (c.delNodeMaps[k + 3])
			scene.materialForNode[id] = c.delNodeMaps[k + 4];
		m_deletedNodes.erase(id);
	}
	if (c.number >= 0.0 && c.number < double(m_world->lightLocations.size()))
		scene.lightForNode[n] = uint32_t(c.number);
	if (c.beforeFlag)
		scene.cvolForNode[n] = ::SceneCvol{ uint32_t(c.beforeVec4.x),
			c.beforeVec4.y, c.beforeVec4.z, c.beforeVec4.w };
	std::vector<size_t> order;
	for (size_t k = 0; k + 4 < c.delDrawItems.size(); k += 5)
		order.push_back(k);
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		return c.delDrawItems[a] < c.delDrawItems[b];
	});
	for (size_t k : order) {
		const uint32_t pos = c.delDrawItems[k];
		smvk::World::RenderItem item{ c.delDrawItems[k + 1],
			c.delDrawItems[k + 2], c.delDrawItems[k + 3],
			c.delDrawItems[k + 4] };
		m_world->drawItems.insert(m_world->drawItems.begin()
			+ ptrdiff_t(pos), item);
		m_world->itemFlags.insert(m_world->itemFlags.begin()
			+ ptrdiff_t(pos), item.flags);
	}
	m_world->flagLists.clear();
	auto git = m_terrainGroups.find(n);
	if (git != m_terrainGroups.end())
		git->second.deleted = false; // restored: paints/rebuilds live again
	m_selected = n; // like creates: restored content selects
	Log("Undid delete: '" + getNodeName(scene, int(n)) + "' (#" +
		std::to_string(n) + ") restored.");
}

void EditorApp::DeleteSelection() {
	if (!m_world) {
		Log("Delete: no scene open.");
		return;
	}
	if (m_worldSelected) {
		Log("Delete: 'World' holds global settings (not a scene node).");
		return;
	}
	std::vector<uint32_t> targets(m_selection);
	if (targets.empty() && m_selected != UINT32_MAX)
		targets.push_back(m_selected);
	if (targets.empty()) {
		Log("Delete: nothing selected.");
		return;
	}
	int deleted = 0;
	std::vector<uint32_t> handled; // ids dispatched (dedupe tiles/groups)
	auto dispatch = [&](uint32_t id) {
		for (uint32_t h : handled)
			if (h == id)
				return false;
		handled.push_back(id);
		EditCmd c{};
		c.verb = EditVerb::Delete;
		c.node = id;
		Do(c);
		++deleted;
		return true;
	};
	for (uint32_t n : targets) {
		if (n == UINT32_MAX || n >= m_world->scene.hierarchy.size())
			continue;
		if (m_deletedNodes.contains(n))
			continue; // already deleted
		if (n == 0) {
			Log("Delete: the scene root stays (delete children instead).");
			continue;
		}
		if (IsRefPlaceholder(n)) {
			// The placeholder IS the reference binding anchor: dropping
			// it would orphan the linked scene on reload. Maya parity:
			// remove/unload the reference, not its node.
			Log("Delete: '" + getNodeName(m_world->scene, int(n)) +
				"' is a reference placeholder — remove the reference "
				"itself (References panel).");
			continue;
		}
		// Terrain TILE (viewport picks select tiles — the group node
		// carries no mesh): resolve to the GROUP. Deleting part of a
		// terrain deletes the terrain.
		const uint32_t group = TerrainGroupOf(n);
		if (group != UINT32_MAX && !m_terrainGroups.contains(n)) {
			auto g = m_terrainGroups.find(group);
			if (g != m_terrainGroups.end() && !g->second.deleted
				&& !m_deletedNodes.contains(group) && dispatch(group))
				Log("Delete: tile selection resolves to terrain group #"
					+ std::to_string(group) + ".");
			continue;
		}
		// Any other node deletes (created lights, created terrain
		// groups, imported meshes/lights/suns, plain transforms).
		auto git = m_terrainGroups.find(n);
		if (git != m_terrainGroups.end() && git->second.deleted)
			continue; // group already deleted (its tiles may still be selected)
		dispatch(n);
	}
	if (deleted > 0)
		Log("Delete: removed " + std::to_string(deleted) + " node"
			+ (deleted > 1 ? "s" : "") + ".");
}

// ---------------------------------------------------------------------------
// CreateTransform: plain transform nodes (Maya group locators). No mesh,
// no light, no pool entry — the reference system uses them as
// displacement placeholders for created content parented under a
// reference that gets disabled (see editor_references.cpp).
// ---------------------------------------------------------------------------
void EditorApp::ExecCreateTransform(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	uint32_t parent = c.node2;
	if (parent == UINT32_MAX || parent >= scene.hierarchy.size())
		parent = DefaultCreateParent();
	if (parent != UINT32_MAX && parent >= scene.hierarchy.size())
		return;
	const int level = (parent == UINT32_MAX) ? 0
		: scene.hierarchy[parent].level + 1;
	const int created = addNode(scene,
		parent == UINT32_MAX ? -1 : int(parent), level);
	if (created < 0)
		return;
	c.node = uint32_t(created);
	// TRS rebuild (editor convention: Rz*Ry*Rx, degrees).
	glm::mat4 m(1.0f);
	m = glm::translate(m, c.afterVec);
	m = glm::rotate(m, glm::radians(c.vec4.z), glm::vec3(0, 0, 1));
	m = glm::rotate(m, glm::radians(c.vec4.y), glm::vec3(0, 1, 0));
	m = glm::rotate(m, glm::radians(c.vec4.x), glm::vec3(1, 0, 0));
	m = glm::scale(m, c.svec);
	scene.localTransform[size_t(created)] = m;
	const uint32_t nameId = uint32_t(scene.nodeNames.size());
	scene.nodeNames.push_back(c.afterText);
	scene.nameForNode[uint32_t(created)] = nameId;
	markAsChanged(scene, created);
	recalculateGlobalTransforms(scene);
	m_selected = uint32_t(created);
	Log("Created transform '" + c.afterText + "' (#"
		+ std::to_string(created) + ").");
}

void EditorApp::ExecUncreateTransform(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	// A transform node carries no links: physical pop when it is still
	// the childless tail (created-light precedent), else hide with ids
	// kept stable.
	const bool tail = (n + 1 == uint32_t(scene.hierarchy.size()));
	const bool childless = scene.hierarchy[n].firstChild < 0;
	if (tail && childless) {
		scene.nameForNode.erase(n);
		const int p = scene.hierarchy[n].parent;
		if (p >= 0 && size_t(p) < scene.hierarchy.size()) {
			auto& hp = scene.hierarchy[size_t(p)];
			if (hp.firstChild == int(n)) {
				hp.firstChild = scene.hierarchy[n].nextSibling;
			} else {
				int prev = hp.firstChild;
				while (prev >= 0
					&& scene.hierarchy[size_t(prev)].nextSibling != int(n))
					prev = scene.hierarchy[size_t(prev)].nextSibling;
				if (prev >= 0)
					scene.hierarchy[size_t(prev)].nextSibling =
						scene.hierarchy[n].nextSibling;
			}
			int tailId = -1;
			for (int s = hp.firstChild; s >= 0;
				s = scene.hierarchy[size_t(s)].nextSibling)
				tailId = s;
			if (hp.firstChild >= 0)
				scene.hierarchy[size_t(hp.firstChild)].lastSibling = tailId;
		}
		scene.localTransform.pop_back();
		scene.globalTransform.pop_back();
		scene.hierarchy.pop_back();
		if (scene.staticRoot.size() == n + 1)
			scene.staticRoot.pop_back();
		if (scene.staticNode.size() == n + 1)
			scene.staticNode.pop_back();
		if (scene.noLightmap.size() == n + 1)
			scene.noLightmap.pop_back();
		if (scene.ditherCloseup.size() == n + 1)
			scene.ditherCloseup.pop_back();
		if (scene.hidden.size() == n + 1)
			scene.hidden.pop_back();
		recalculateGlobalTransforms(scene);
		if (m_selected == n)
			m_selected = (p >= 0) ? uint32_t(p) : UINT32_MAX;
		Log("Undid transform creation (#" + std::to_string(n) + ").");
	} else {
		MarkHidden(scene, int(n));
		Log("Undid transform creation (#" + std::to_string(n)
			+ "): hidden, ids kept stable.");
	}
}

bool EditorApp::ReadNodeLightAttr(uint32_t node, const std::string& attr,
	glm::vec4& v4, double& num, bool& flag, std::string& text) const {
	v4 = glm::vec4(0.0f);
	num = 0.0;
	flag = false;
	text.clear();
	const ::SceneLight* L = FindNodeLight(node, nullptr);
	if (!L)
		return false;
	if (attr == "color") {
		v4 = glm::vec4(L->color, 1.0f);
	} else if (attr == "intensity") {
		num = double(L->intensity);
	} else if (attr == "range") {
		num = double(L->range);
	} else if (attr == "inner") {
		num = double(glm::degrees(L->innerConeAngle));
	} else if (attr == "outer") {
		num = double(glm::degrees(L->outerConeAngle));
	} else if (attr == "static") {
		flag = (L->flags & ::SceneLight::kSceneLightFlag_Static) != 0;
	} else if (attr == "shadow") {
		flag = (L->flags & ::SceneLight::kSceneLightFlag_CastShadow) != 0;
	} else if (attr == "name") {
		text = L->name;
	} else if (attr == "position" || attr == "rotation") {
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return false;
		glm::vec3 t, r, s;
		ReadLocalTRS(node, t, r, s);
		v4 = glm::vec4(attr == "position" ? t : r, 1.0f);
	} else {
		return false;
	}
	return true;
}

bool EditorApp::WriteNodeLightAttr(uint32_t node, const std::string& attr,
	const glm::vec4& v4, double num, bool flag, const std::string& text) {
	if (!m_world)
		return false;
	Scene& scene = m_world->scene;
	auto lit = scene.lightForNode.find(node);
	if (lit == scene.lightForNode.end()
		|| lit->second >= m_world->lightLocations.size())
		return false;
	const auto& loc = m_world->lightLocations[lit->second];
	if (!loc.cluster)
		return false;
	::SceneLight* L = loc.cluster->lights.get(loc.handle);
	if (!L)
		return false;
	if (attr == "color") {
		L->color = glm::vec3(v4.x, v4.y, v4.z);
	} else if (attr == "intensity") {
		L->intensity = std::max(float(num), 0.0f);
	} else if (attr == "range") {
		L->range = std::max(float(num), 0.0f);
	} else if (attr == "inner") {
		L->innerConeAngle = glm::radians(std::clamp(float(num), 0.0f, 89.0f));
	} else if (attr == "outer") {
		L->outerConeAngle = glm::radians(std::clamp(float(num), 0.0f, 89.0f));
	} else if (attr == "static") {
		if (flag)
			L->flags |= ::SceneLight::kSceneLightFlag_Static;
		else
			L->flags &= ~::SceneLight::kSceneLightFlag_Static;
	} else if (attr == "shadow") {
		if (flag)
			L->flags |= ::SceneLight::kSceneLightFlag_CastShadow;
		else
			L->flags &= ~::SceneLight::kSceneLightFlag_CastShadow;
	} else if (attr == "name") {
		L->name = text;
		const uint32_t id = uint32_t(scene.nodeNames.size());
		scene.nodeNames.push_back(text);
		scene.nameForNode[node] = id;
	} else if (attr == "position" || attr == "rotation") {
		if (node >= scene.hierarchy.size())
			return false;
		glm::vec3 t, r, s;
		ReadLocalTRS(node, t, r, s);
		if (attr == "position")
			t = glm::vec3(v4.x, v4.y, v4.z);
		else
			r = glm::vec3(v4.x, v4.y, v4.z);
		scene.localTransform[node] = LocalMatrixFromTRS(t, r, s);
		markAsChanged(scene, int(node));
		recalculateGlobalTransforms(scene);
	} else {
		return false;
	}
	return true;
}

bool EditorApp::ReadNodeCvolAttr(uint32_t node, const std::string& attr,
	glm::vec4& v4, double& num, std::string& text) const {
	v4 = glm::vec4(0.0f);
	num = 0.0;
	text.clear();
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return false;
	const Scene& scene = m_world->scene;
	auto it = scene.cvolForNode.find(node);
	if (it == scene.cvolForNode.end())
		return false;
	const ::SceneCvol& def = it->second;
	if (attr == "priority") {
		num = double(def.priority);
	} else if (attr == "spacing") {
		num = double(def.spacing);
	} else if (attr == "inside") {
		num = double(def.insideFade);
	} else if (attr == "outside") {
		num = double(def.outsideFade);
	} else if (attr == "name") {
		text = getNodeName(scene, int(node));
	} else if (attr == "position" || attr == "rotation"
		|| attr == "size") {
		glm::vec3 t, r, s;
		ReadLocalTRS(node, t, r, s);
		v4 = glm::vec4(attr == "position" ? t : (attr == "rotation" ? r : s),
			1.0f);
	} else {
		return false;
	}
	return true;
}

bool EditorApp::WriteNodeCvolAttr(uint32_t node, const std::string& attr,
	const glm::vec4& v4, double num, const std::string& text) {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return false;
	Scene& scene = m_world->scene;
	auto it = scene.cvolForNode.find(node);
	if (it == scene.cvolForNode.end())
		return false;
	::SceneCvol& def = it->second;
	if (attr == "priority") {
		def.priority = uint32_t(std::max(num, 0.0));
	} else if (attr == "spacing") {
		def.spacing = std::max(float(num), 1e-3f);
	} else if (attr == "inside") {
		def.insideFade = std::max(float(num), 0.0f);
	} else if (attr == "outside") {
		def.outsideFade = std::max(float(num), 0.0f);
	} else if (attr == "name") {
		const uint32_t id = uint32_t(scene.nodeNames.size());
		scene.nodeNames.push_back(text);
		scene.nameForNode[node] = id;
	} else if (attr == "position" || attr == "rotation"
		|| attr == "size") {
		glm::vec3 t, r, s;
		ReadLocalTRS(node, t, r, s);
		if (attr == "position")
			t = glm::vec3(v4.x, v4.y, v4.z);
		else if (attr == "rotation")
			r = glm::vec3(v4.x, v4.y, v4.z);
		else
			s = glm::vec3(std::max(v4.x, 1e-3f), std::max(v4.y, 1e-3f),
				std::max(v4.z, 1e-3f));
		scene.localTransform[node] = LocalMatrixFromTRS(t, r, s);
		markAsChanged(scene, int(node));
		recalculateGlobalTransforms(scene);
	} else {
		return false;
	}
	return true;
}

bool EditorApp::CvolBox(uint32_t node, glm::vec3& mn, glm::vec3& mx) const {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return false;
	const Scene& scene = m_world->scene;
	if (!scene.cvolForNode.contains(node))
		return false;
	if (node >= scene.globalTransform.size())
		return false;
	// Authoring box: center = global translate, full extents = global
	// scale (basis lengths — exact for unrotated nodes, a conservative
	// AABB otherwise). Rotation is ignored by design (gi_baker
	// box-spec shape).
	const glm::mat4& g = scene.globalTransform[node];
	const glm::vec3 c(g[3]);
	const glm::vec3 half(0.5f * glm::vec3(glm::length(g[0]),
		glm::length(g[1]), glm::length(g[2])));
	mn = c - half;
	mx = c + half;
	return true;
}

void EditorApp::Do(EditCmd c) {
	ExecCmd(c, true);
	if (c.verb == EditVerb::SetVisibility)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Visibility);
	else if (c.verb == EditVerb::AssignMaterial)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Material);
	else if (c.verb == EditVerb::SetNoLightmap)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::NoLightmap);
	else if (c.verb == EditVerb::SetDitherCloseup)
		NoteLocalOverrideWritten(c.node,
			smvk::tour::OverrideAttr::DitherCloseup);
	m_history.erase(m_history.begin() + m_historyCursor, m_history.end());
	m_history.push_back(c);
	m_historyCursor = m_history.size();
	while (m_history.size() > kHistoryCap) {
		m_history.erase(m_history.begin());
		--m_historyCursor;
	}
	AutosaveTour();
}

void EditorApp::PushDone(EditCmd c) {
	// Already executed live (drag path): just record + save.
	if (c.verb == EditVerb::SetVisibility)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Visibility);
	else if (c.verb == EditVerb::AssignMaterial)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Material);
	else if (c.verb == EditVerb::SetNoLightmap)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::NoLightmap);
	else if (c.verb == EditVerb::SetDitherCloseup)
		NoteLocalOverrideWritten(c.node,
			smvk::tour::OverrideAttr::DitherCloseup);
	m_history.erase(m_history.begin() + m_historyCursor, m_history.end());
	m_history.push_back(c);
	m_historyCursor = m_history.size();
	while (m_history.size() > kHistoryCap) {
		m_history.erase(m_history.begin());
		--m_historyCursor;
	}
	AutosaveTour();
}

bool EditorApp::Undo() {
	if (m_historyCursor == 0)
		return false;
	--m_historyCursor;
	ExecCmd(m_history[m_historyCursor], false);
	ReassertWideOverrides(m_history[m_historyCursor].node);
	AutosaveTour();
	return true;
}

bool EditorApp::Redo() {
	if (m_historyCursor >= m_history.size())
		return false;
	ExecCmd(m_history[m_historyCursor], true);
	const EditCmd& c = m_history[m_historyCursor];
	if (c.verb == EditVerb::SetVisibility)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Visibility);
	else if (c.verb == EditVerb::AssignMaterial)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Material);
	else if (c.verb == EditVerb::SetNoLightmap)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::NoLightmap);
	else if (c.verb == EditVerb::SetDitherCloseup)
		NoteLocalOverrideWritten(c.node,
			smvk::tour::OverrideAttr::DitherCloseup);
	++m_historyCursor;
	AutosaveTour();
	return true;
}

void EditorApp::ClearHistory() {
	m_history.clear();
	m_historyCursor = 0;
}

// Save-time coalescing (see SaveTourTo): the saved .tour keeps only
// the state that survives — last occurrence per class at its original
// position — so toggle pairs like hide/show/hide collapse to the final
// state in the FILE. Same classes as CoalesceHistory; structural
// commands (creates, paints, deletes, uuids) always stay. The live
// stack, undo and the autosave are untouched (temporal memory keeps
// everything).
std::vector<size_t> EditorApp::CoalescedSaveIndices() const {
	std::map<std::pair<int, uint32_t>, size_t> lastByNode;
	std::map<std::tuple<int, uint32_t, std::string>, size_t> lastByAttr;
	for (size_t i = 0; i < m_historyCursor; ++i) {
		const EditCmd& c = m_history[i];
		switch (c.verb) {
		case EditVerb::Rename:
		case EditVerb::Translate:
		case EditVerb::Rotate:
		case EditVerb::Scale:
		case EditVerb::Parent:
		case EditVerb::SetVisibility:
		case EditVerb::SetNoLightmap:
		case EditVerb::SetDitherCloseup:
		case EditVerb::AssignMaterial:
			lastByNode[{ int(c.verb), c.node }] = i;
			break;
		case EditVerb::SetMatAttr:
			lastByAttr[{ int(c.verb), c.matWorld, c.attr }] = i;
			break;
		case EditVerb::SetLightAttr:
			lastByAttr[{ int(c.verb), c.node, c.lightAttr }] = i;
			break;
		case EditVerb::SetCvolAttr:
			lastByAttr[{ int(c.verb), c.node, c.cvolAttr }] = i;
			break;
		default:
			break;
		}
	}
	std::vector<size_t> kept;
	kept.reserve(m_historyCursor);
	for (size_t i = 0; i < m_historyCursor; ++i) {
		const EditCmd& c = m_history[i];
		bool superseded = false;
		switch (c.verb) {
		case EditVerb::Rename:
		case EditVerb::Translate:
		case EditVerb::Rotate:
		case EditVerb::Scale:
		case EditVerb::Parent:
		case EditVerb::SetVisibility:
		case EditVerb::SetNoLightmap:
		case EditVerb::SetDitherCloseup:
		case EditVerb::AssignMaterial:
			superseded = lastByNode[{ int(c.verb), c.node }] != i;
			break;
		case EditVerb::SetMatAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.matWorld, c.attr }] != i;
			break;
		case EditVerb::SetLightAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.node, c.lightAttr }] != i;
			break;
		case EditVerb::SetCvolAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.node, c.cvolAttr }] != i;
			break;
		default:
			break;
		}
		if (!superseded)
			kept.push_back(i);
	}
	return kept;
}

size_t EditorApp::CoalesceHistory() {
	if (m_historyCursor < 2)
		return 0;
	// Last-occurrence collapse. Every kept command stays at its ORIGINAL
	// position; only superseded same-key predecessors (and the redo tail)
	// drop. Correctness rests on two replay properties: (1) every
	// collapsible class is last-wins — the tour's transform pass collects
	// per-channel triples and attr/material assignments overwrite — so an
	// earlier same-key command contributes nothing once a later one
	// exists; (2) keeping the LAST occurrence at its own position
	// preserves the replay-time context (recorded verify names, refs)
	// exactly as the mid-history state looked when that command ran.
	//
	// Creates and terrain paint stamps always stay: creates are structure
	// (later -ref addressing counts them) and paint stamps are an ordered
	// cumulative bake.
	//
	// Key classes:
	//   (verb, node)              Rename/Translate/Rotate/Scale/Parent/
	//                              SetVisibility/SetNoLightmap/
	//                              SetDitherCloseup/AssignMaterial
	//   (SetMatAttr, mat, attr)    shared material value edits
	//   (SetLightAttr, node, attr) light property edits
	//   (SetCvolAttr, node, attr)  convex-volume property edits
	std::map<std::pair<int, uint32_t>, size_t> lastByNode;
	std::map<std::tuple<int, uint32_t, std::string>, size_t> lastByAttr;
	for (size_t i = 0; i < m_historyCursor; ++i) {
		const EditCmd& c = m_history[i];
		switch (c.verb) {
		case EditVerb::Rename:
		case EditVerb::Translate:
		case EditVerb::Rotate:
		case EditVerb::Scale:
		case EditVerb::Parent:
		case EditVerb::SetVisibility:
		case EditVerb::SetNoLightmap:
		case EditVerb::SetDitherCloseup:
		case EditVerb::AssignMaterial:
			lastByNode[{ int(c.verb), c.node }] = i;
			break;
		case EditVerb::SetMatAttr:
			lastByAttr[{ int(c.verb), c.matWorld, c.attr }] = i;
			break;
		case EditVerb::SetLightAttr:
			lastByAttr[{ int(c.verb), c.node, c.lightAttr }] = i;
			break;
		case EditVerb::SetCvolAttr:
			lastByAttr[{ int(c.verb), c.node, c.cvolAttr }] = i;
			break;
		default:
			break; // Create*/TerrainPaint: always keep
		}
	}
	std::vector<EditCmd> kept;
	kept.reserve(m_historyCursor);
	size_t dropped = m_history.size() - m_historyCursor; // redo tail
	for (size_t i = 0; i < m_historyCursor; ++i) {
		const EditCmd& c = m_history[i];
		bool superseded = false;
		switch (c.verb) {
		case EditVerb::Rename:
		case EditVerb::Translate:
		case EditVerb::Rotate:
		case EditVerb::Scale:
		case EditVerb::Parent:
		case EditVerb::SetVisibility:
		case EditVerb::SetNoLightmap:
		case EditVerb::SetDitherCloseup:
		case EditVerb::AssignMaterial:
			superseded = lastByNode[{ int(c.verb), c.node }] != i;
			break;
		case EditVerb::SetMatAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.matWorld, c.attr }] != i;
			break;
		case EditVerb::SetLightAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.node, c.lightAttr }] != i;
			break;
		case EditVerb::SetCvolAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.node, c.cvolAttr }] != i;
			break;
		default:
			break;
		}
		if (superseded)
			++dropped;
		else
			kept.push_back(c);
	}
	m_history = std::move(kept);
	m_historyCursor = m_history.size();
	SaveTour();
	return dropped;
}

size_t EditorApp::UndoDepth() const {
	return m_historyCursor;
}

size_t EditorApp::RedoDepth() const {
	return m_history.size() - m_historyCursor;
}

static std::string VerbName(editor::EditVerb v) {
	switch (v) {
	case editor::EditVerb::Rename: return "rename";
	case editor::EditVerb::Translate: return "move";
	case editor::EditVerb::Rotate: return "rotate";
	case editor::EditVerb::Scale: return "scale";
	case editor::EditVerb::AssignMaterial: return "assign material";
	case editor::EditVerb::SetMatAttr: return "material edit";
	case editor::EditVerb::SetNoLightmap: return "lightmap flag";
	case editor::EditVerb::SetDitherCloseup: return "dither flag";
	case editor::EditVerb::SetVisibility: return "visibility";
	case editor::EditVerb::Parent: return "reparent";
	case editor::EditVerb::CreateLight: return "create light";
	case editor::EditVerb::SetLightAttr: return "light edit";
	case editor::EditVerb::CreateTerrain: return "create terrain";
	case editor::EditVerb::TerrainPaint: return "terrain paint";
	case editor::EditVerb::Delete: return "delete";
	case editor::EditVerb::CreateTransform: return "create transform";
	case editor::EditVerb::CreateCVolume: return "create cvolume";
	case editor::EditVerb::SetCvolAttr: return "cvolume edit";
	}
	return "?";
}

std::string EditorApp::UndoLabel() const {
	if (m_historyCursor == 0)
		return "";
	const EditCmd& c = m_history[m_historyCursor - 1];
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%s #%u", VerbName(c.verb).c_str(), c.node);
	return buf;
}

std::string EditorApp::RedoLabel() const {
	if (m_historyCursor >= m_history.size())
		return "";
	const EditCmd& c = m_history[m_historyCursor];
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%s #%u", VerbName(c.verb).c_str(), c.node);
	return buf;
}

// ---- tour translation ----

bool EditorApp::CmdToTour(const EditorApp& app, const EditCmd& c,
	smvk::tour::Command& out) {
	const int cn = app.WorldToCluster(c.node);
	auto needNode = [&]() {
		if (cn < 0)
			return false;
		out.node = uint32_t(cn);
		out.name = getNodeName(app.m_world->scene, int(c.node));
		return true;
	};
	// Symbolic -p form for a parent (world id): recorded uuid hex when
	// present (covers UNNAMED parents), else the shortest-unique
	// name/path when named, else empty (numeric -p fallback). Follows
	// the tour's string/dag rule: names first, uuids for the unnamed.
	auto parentRefFor = [&](uint32_t worldParent) {
		if (!app.m_world
			|| worldParent >= app.m_world->scene.hierarchy.size())
			return std::string();
		const Scene& scene = app.m_world->scene;
		const std::string nm = getNodeName(scene, int(worldParent));
		if (!nm.empty()) {
			const std::string p =
				smvk::tour::DagShortestUniquePath(scene, worldParent);
			if (!p.empty())
				return p;
		}
		auto it = scene.uuidForNode.find(worldParent);
		if (it != scene.uuidForNode.end())
			return it->second;
		return std::string();
	};
	switch (c.verb) {
	case EditVerb::Rename: {
		const int k = app.CreatedLightIndex(c.node);
		if (k >= 0) {
			out.verb = smvk::tour::Verb::SetLightAttr;
			out.node = smvk::tour::kCreateLightRefBase | uint32_t(k);
			out.text = "name";
			out.name = c.afterText;
			return true;
		}
		// Created-cvolume nodes own no cluster id either: renames
		// persist as cvolume-attr lines (-ref addressing).
		const int kv = app.CreatedCvolIndex(c.node);
		if (kv >= 0) {
			out.verb = smvk::tour::Verb::SetCvolAttr;
			out.node = smvk::tour::kCreateCvolRefBase | uint32_t(kv);
			out.text = "name";
			out.name = c.afterText;
			return true;
		}
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::Rename;
		out.text = c.afterText;
		return true;
	}
	case EditVerb::Translate:
	case EditVerb::Rotate: {
		// Created-light nodes own no cluster id: persist moves/aims as
		// light-attr triples (-ref addressing) so gizmo edits survive.
		const int k = app.CreatedLightIndex(c.node);
		if (k >= 0) {
			out.verb = smvk::tour::Verb::SetLightAttr;
			out.node = smvk::tour::kCreateLightRefBase | uint32_t(k);
			out.text = (c.verb == EditVerb::Translate) ? "position" : "rotation";
			out.vec4 = glm::vec4(c.afterVec, 1.0f);
			return true;
		}
		// Created-cvolume nodes: same treatment (cvolume-attr triples).
		const int kv = app.CreatedCvolIndex(c.node);
		if (kv >= 0) {
			out.verb = smvk::tour::Verb::SetCvolAttr;
			out.node = smvk::tour::kCreateCvolRefBase | uint32_t(kv);
			out.text = (c.verb == EditVerb::Translate) ? "position" : "rotation";
			out.vec4 = glm::vec4(c.afterVec, 1.0f);
			return true;
		}
		if (!needNode())
			return false;
		out.verb = (c.verb == EditVerb::Translate)
			? smvk::tour::Verb::SetTranslate : smvk::tour::Verb::SetRotate;
		out.vec = c.afterVec;
		return true;
	}
	case EditVerb::Scale: {
		// Created-cvolume size edits ride the size channel (-ref).
		const int kv = app.CreatedCvolIndex(c.node);
		if (kv >= 0) {
			out.verb = smvk::tour::Verb::SetCvolAttr;
			out.node = smvk::tour::kCreateCvolRefBase | uint32_t(kv);
			out.text = "size";
			out.vec4 = glm::vec4(c.afterVec, 1.0f);
			return true;
		}
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::SetScale;
		out.vec = c.afterVec;
		return true;
	}
	case EditVerb::AssignMaterial: {
		if (!needNode())
			return false;
		const int pool = app.WorldMatToCluster(c.material);
		if (pool < 0)
			return false;
		out.verb = smvk::tour::Verb::AssignMaterial;
		out.material = uint32_t(pool);
		return true;
	}
	case EditVerb::SetMatAttr: {
		const int pool = app.WorldMatToCluster(c.matWorld);
		if (pool < 0)
			return false;
		out.verb = smvk::tour::Verb::SetMatAttr;
		out.material = uint32_t(pool);
		out.text = c.attr;
		out.vec4 = c.vec4;
		out.number = c.number;
		// "layers" rides vec4 as WORLD material ids in the editor; the
		// tour file speaks cluster pool ids (the cook domain).
		if (c.attr == "layers" && app.m_world) {
			auto toCluster = [&](int worldId) -> float {
				if (worldId < 0)
					return -1.0f;
				const int p = app.WorldMatToCluster(uint32_t(worldId));
				return (p >= 0) ? float(p) : -1.0f;
			};
			out.vec4.x = toCluster(int(c.vec4.x));
			out.vec4.y = toCluster(int(c.vec4.y));
			out.vec4.z = toCluster(int(c.vec4.z));
		}
		return true;
	}
	case EditVerb::SetNoLightmap:
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::SetNoLightmap;
		out.flag = c.afterFlag;
		return true;
	case EditVerb::SetDitherCloseup:
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::SetDitherCloseup;
		out.flag = c.afterFlag;
		return true;
	case EditVerb::SetVisibility:
		if (!needNode())
			return false;
		// Reference placeholders are owned by the reference line's
		// `enabled` bool — their visibility never persists as a
		// command (this also purges legacy placeholder lines).
		if (app.IsRefPlaceholder(c.node))
			return false;
		out.verb = smvk::tour::Verb::SetVisibility;
		out.flag = c.afterFlag;
		return true;
	case EditVerb::Parent: {
		if (!needNode())
			return false;
		const int pn = app.WorldToCluster(c.node2);
		if (pn < 0)
			return false;
		out.verb = smvk::tour::Verb::Parent;
		out.node2 = uint32_t(pn);
		out.parentRef = parentRefFor(c.node2);
		out.name = getNodeName(app.m_world->scene, int(c.node));
		return true;
	}
	case EditVerb::CreateLight: {
		// Emit the LIVE create state: the node may have moved and its
		// params may have been attr-edited after creation (those later
		// SetLightAttr commands re-emit harmlessly and idempotently).
		const uint32_t created = c.node;
		if (created == UINT32_MAX
			|| created >= app.m_world->scene.hierarchy.size())
			return false;
		const ::SceneLight* L = app.FindNodeLight(created, nullptr);
		if (!L)
			return false; // detached (tombstoned): nothing to persist
		int pp = app.WorldToCluster(c.node2);
		if (pp < 0) {
			// World root has no cluster id: the primary cluster's root
			// (id 0) is the replay-stable equivalent.
			if (c.node2 == 0)
				pp = 0;
			else
				return false;
		}
		const glm::vec3 t = glm::vec3(
			app.m_world->scene.localTransform[created][3]);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = (L->type == ::SceneLight::Type::Spot)
			? "spotLight" : "pointLight";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(c.node2);
		out.text = L->name;
		out.vec = t;
		out.vec4 = glm::vec4(
			L->type == ::SceneLight::Type::Spot ? 2.0f : 1.0f,
			L->intensity, L->range, glm::degrees(L->innerConeAngle));
		out.number = double(glm::degrees(L->outerConeAngle));
		out.flag = (L->flags & ::SceneLight::kSceneLightFlag_Static) != 0;
		out.node2 = (L->flags & ::SceneLight::kSceneLightFlag_CastShadow) != 0
			? 1u : 0u;
		return true;
	}
	case EditVerb::CreateTerrain: {
		// Emit the LIVE create state: the group may have moved and its
		// tiles may have been re-material-edited after creation (those
		// later AssignMaterial/SetTranslate commands ride along).
		const uint32_t created = c.node;
		if (created == UINT32_MAX
			|| created >= app.m_world->scene.hierarchy.size())
			return false;
		// Deleted (tombstoned) group: drop the creation from the file
		// entirely — the created-light detached precedent.
		{
			auto git = app.m_terrainGroups.find(created);
			if (git != app.m_terrainGroups.end() && git->second.deleted)
				return false;
		}
		int pp = app.WorldToCluster(c.beforeParent);
		if (pp < 0) {
			// World root has no cluster id: the primary cluster's root
			// (id 0) is the replay-stable equivalent.
			if (c.beforeParent == 0)
				pp = 0;
			else
				return false;
		}
		const glm::vec3 t = glm::vec3(
			app.m_world->scene.localTransform[created][3]);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = "terrain";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(c.beforeParent);
		out.text = c.afterText;
		out.vec = t;
		out.vec4 = c.vec4;         // (size, res, height, freq)
		out.number = c.number;    // seed
		out.node2 = c.node2;       // tilesX | (tilesZ << 16)
		const int pm = app.WorldMatToCluster(c.material);
		// Unmappable (editor default / none) emits a sentinel; the cook
		// mints the same deterministic default material on miss.
		out.material = (pm >= 0) ? uint32_t(pm) : 0xFFFFFFFFu;
		return true;
	}
	case EditVerb::CreateTransform: {
		// Emit the LIVE create state (transform nodes carry no links,
		// so a deleted one must not emit — same tombstone contract as
		// the detached-light check).
		const uint32_t created = c.node;
		if (created == UINT32_MAX
			|| created >= app.m_world->scene.hierarchy.size())
			return false;
		if (app.m_deletedNodes.contains(created))
			return false;
		int pp = app.WorldToCluster(c.node2);
		if (pp < 0) {
			if (c.node2 == 0)
				pp = 0;
			else
				return false;
		}
		glm::vec3 t, r, s;
		app.ReadLocalTRS(created, t, r, s);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = "transform";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(c.node2);
		out.text = getNodeName(app.m_world->scene, int(created));
		out.vec = t;
		out.vec4 = glm::vec4(r, 0.0f);
		out.svec = s;
		return true;
	}
	case EditVerb::TerrainPaint: {
		// Every paint stamp is tour state (deterministic replay: the seed
		// grid + ordered stamps rebuild identical heights).
		const int k = app.CreatedTerrainIndex(c.node);
		if (k < 0)
			return false;
		out.verb = smvk::tour::Verb::TerrainPaint;
		out.node = smvk::tour::kCreateTerrainRefBase | uint32_t(k);
		out.text = c.terrainOp;
		out.vec4 = c.vec4;      // (worldX, worldZ, radius, target)
		out.number = c.number;  // strength
		return true;
	}
	case EditVerb::SetLightAttr: {
		const int k = app.CreatedLightIndex(c.node);
		if (k >= 0) {
			out.node = smvk::tour::kCreateLightRefBase | uint32_t(k);
		} else if (!needNode()) {
			return false;
		}
		out.verb = smvk::tour::Verb::SetLightAttr;
		out.text = c.lightAttr;
		if (c.lightAttr == "color" || c.lightAttr == "position"
			|| c.lightAttr == "rotation") {
			out.vec4 = c.vec4;
		} else if (c.lightAttr == "name") {
			out.name = c.afterText;
		} else if (c.lightAttr == "static" || c.lightAttr == "shadow") {
			out.flag = c.afterFlag;
		} else {
			out.number = c.number;
		}
		return true;
	}
	case EditVerb::CreateCVolume: {
		// Emit the LIVE create state: the node may have moved/scaled
		// and its params may have been attr-edited after creation
		// (those later SetCvolAttr commands re-emit harmlessly and
		// idempotently — the light precedent).
		const uint32_t created = c.node;
		if (created == UINT32_MAX
			|| created >= app.m_world->scene.hierarchy.size())
			return false;
		auto cvit = app.m_world->scene.cvolForNode.find(created);
		if (cvit == app.m_world->scene.cvolForNode.end())
			return false; // detached (tombstoned): nothing to persist
		if (app.m_deletedNodes.contains(created))
			return false;
		int pp = app.WorldToCluster(c.node2);
		if (pp < 0) {
			if (c.node2 == 0)
				pp = 0;
			else
				return false;
		}
		glm::vec3 t, r, s;
		app.ReadLocalTRS(created, t, r, s);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = "cvolume";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(c.node2);
		out.text = getNodeName(app.m_world->scene, int(created));
		out.vec = t;
		out.svec = glm::vec3(std::max(s.x, 1e-3f),
			std::max(s.y, 1e-3f), std::max(s.z, 1e-3f));
		out.vec4 = glm::vec4(float(cvit->second.priority),
			cvit->second.spacing, cvit->second.insideFade,
			cvit->second.outsideFade);
		return true;
	}
	case EditVerb::SetCvolAttr: {
		const int k = app.CreatedCvolIndex(c.node);
		if (k >= 0) {
			out.node = smvk::tour::kCreateCvolRefBase | uint32_t(k);
		} else if (!needNode()) {
			return false;
		}
		out.verb = smvk::tour::Verb::SetCvolAttr;
		out.text = c.cvolAttr;
		if (c.cvolAttr == "position" || c.cvolAttr == "rotation"
			|| c.cvolAttr == "size") {
			out.vec4 = c.vec4;
		} else if (c.cvolAttr == "name") {
			out.name = c.afterText;
		} else {
			out.number = c.number;
		}
		return true;
	}
	case EditVerb::Delete: {
		// Cluster-backed nodes persist as a first-class Maya delete.
		// Addressing: uuid (file-stable identity, assigned for every
		// delete target by the SaveTourTo pre-pass), name (secondary,
		// via needNode's verify-name side effect), numeric id as the
		// legacy fallback/disambiguator. Editor-created nodes (created
		// lights, cvolumes, terrain groups/tiles — no cluster id) are
		// never written: their creates are dropped by the live-state
		// emission above, removing them from the file entirely.
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::Delete;
		auto uit = app.m_world->scene.uuidForNode.find(c.node);
		if (uit != app.m_world->scene.uuidForNode.end())
			out.uuid = uit->second;
		return true;
	}
	}
	return false;
}

std::string EditorApp::TourPath() const {
	if (m_gltfPath.empty())
		return {};
	// Flintbase scheme scenes keep their identity in the tour; the tour
	// file itself is local working state, so it never lands inside the
	// (immutable) product version folder on storage.
	if (smvk::vfs::HasFlintbasePrefix(m_gltfPath)) {
		std::filesystem::path p(m_gltfHostPath);
		std::filesystem::create_directories("content/tours");
		return ("content/tours/" + p.filename().replace_extension(".tour").string());
	}
	std::filesystem::path p(m_gltfPath);
	p.replace_extension(".tour");
	return p.string();
}

std::string EditorApp::TourTarget() const {
	return m_tourPath.empty() ? TourPath() : m_tourPath;
}

bool EditorApp::SaveTour() {
	const bool ok = SaveTourTo(TourTarget());
	if (ok)
		ClearAutosave();
	return ok;
}

// Autosave maintenance: an explicit save makes the pending autosave
// redundant (Maya drops the autoSave file on manual save).
void EditorApp::ClearAutosave() {
	const std::string path = AutosavePath();
	if (path.empty())
		return;
	std::error_code ec;
	std::filesystem::remove(std::filesystem::path(path), ec);
}

// Autosave timestamp for the recovery dialog (same style as the tour
// header stamp: "Fri, Sep 26, 2026 09:12:04 AM", empty when unknown).
static std::string FileStamp(const std::string& path) {
	namespace fs = std::filesystem;
	std::error_code ec;
	if (path.empty() || !fs::exists(fs::path(path), ec))
		return {};
	const auto ft = fs::last_write_time(fs::path(path), ec);
	if (ec)
		return {};
	const std::time_t tt = std::chrono::system_clock::to_time_t(
		std::chrono::clock_cast<std::chrono::system_clock>(ft));
	char buf[64] = {};
	if (std::tm* lt = std::localtime(&tt))
		std::strftime(buf, sizeof(buf), "%a, %b %d, %Y %I:%M:%S %p", lt);
	return buf;
}

// Autosave recovery offer: when a newer autosave exists for the loaded
// tour (or an autosave with no tour file at all), arm the recovery
// dialog instead of loading anything silently. NORMAL loads never
// touch the autosave content.
void EditorApp::CheckAutosaveOffer() {
	m_showRecoverDialog = false;
	m_recoverAutosave.clear();
	m_recoverTour.clear();
	m_recoverStamp.clear();
	if (!m_world)
		return;
	const std::string tour = TourPath();
	if (tour.empty())
		return;
	if (LatestTourPath(tour) == tour)
		return; // no newer autosave: nothing to offer
	m_showRecoverDialog = true;
	m_recoverAutosave = AutosavePath();
	m_recoverTour = tour;
	m_recoverStamp = FileStamp(m_recoverAutosave);
		Log("Autosave found (" + m_recoverStamp
			+ ") — newer than " + tour + ".");
}

// Per-edit persistence target: the full tour state lands in an autosave
// file under %APPDATA% (never the project's .tour — explicit saves,
// save-as, incremental saves and scene close are the only writers of
// that; see AutosaveTour vs SaveTour). Crash recovery: LoadTour prefers
// the autosave when it is newer than the target file.
std::string EditorApp::AutosavePath() const {
	const std::string target = TourTarget();
	if (target.empty())
		return {};
	namespace fs = std::filesystem;
	const char* appData = std::getenv("APPDATA");
	if (!appData || !*appData)
		appData = std::getenv("HOME");
	if (!appData || !*appData)
		return {};
	fs::path dir = fs::path(appData) / "tourmalineEditor" / "autosave";
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec)
		return {};
	// Stem carries the scene identity (a per-scene autosave slot).
	std::string stem = fs::path(target).stem().string();
	return (dir / (stem + ".tour")).generic_string();
}

bool EditorApp::AutosaveTour() {
	const std::string path = AutosavePath();
	if (path.empty())
		return false;
	// The working doc moved ahead of the last explicit save (every
	// AutosaveTour call site mutates tour state): mark dirty for the
	// window-title star. Explicit saves clear it (SaveTourTo below).
	m_tourDirty = true;
	// Same doc as a real save, but the target stays untouched: reuse
	// the builder through SaveTourTo and restore the previous target.
	const std::string target = m_tourPath;
	const bool ok = SaveTourTo(path);
	m_tourPath = target;
	return ok;
}

std::string EditorApp::LatestTourPath(const std::string& tourPath) const {
	if (tourPath.empty())
		return {};
	const std::string as = AutosavePath();
	if (as.empty())
		return tourPath;
	namespace fs = std::filesystem;
	std::error_code ec;
	const bool asExists = fs::exists(fs::path(as), ec);
	if (!asExists)
		return tourPath;
	if (!fs::exists(fs::path(tourPath), ec))
		return as; // never explicitly saved: the autosave is the doc
	const auto tAs = fs::last_write_time(fs::path(as), ec);
	if (ec)
		return tourPath;
	const auto tTour = fs::last_write_time(fs::path(tourPath), ec);
	if (ec)
		return tourPath;
	return (tAs > tTour) ? as : tourPath;
}

bool EditorApp::SaveTourIncremental() {
	// Maya-style versioning: <stem>.0001.tour, <stem>.0002.tour, ...
	// next to the current target (an existing .NNNN suffix is replaced,
	// so re-incrementing from sponza.0003.tour yields sponza.0004.tour).
	namespace fs = std::filesystem;
	const std::string cur = TourTarget();
	if (cur.empty() || !m_world) {
		Log("Incremental save: no scene.");
		return false;
	}
	const fs::path p(cur);
	const fs::path dir = p.parent_path();
	std::string stem = p.stem().string(); // strips ".tour"
	std::string base = stem;
	if (const size_t dot = stem.find_last_of('.');
		dot != std::string::npos && dot > 0 && dot + 1 < stem.size()) {
		bool allDigits = true;
		for (size_t i = dot + 1; i < stem.size() && allDigits; ++i)
			allDigits = stem[i] >= '0' && stem[i] <= '9';
		if (allDigits)
			base = stem.substr(0, dot);
	}
	uint32_t maxN = 0;
	{
		std::error_code ec;
		const fs::path scanDir = dir.empty() ? fs::path(".") : dir;
		for (fs::directory_iterator it(scanDir, ec), end;
			!ec && it != end; it.increment(ec)) {
			if (!it->is_regular_file(ec))
				continue;
			const fs::path e = it->path();
			if (e.extension() != ".tour")
				continue;
			const std::string s = e.stem().string();
			if (s.size() <= base.size() + 1 ||
				s.compare(0, base.size(), base) != 0 ||
				s[base.size()] != '.')
				continue;
			bool allDigits = true;
			for (size_t i = base.size() + 1; i < s.size() && allDigits; ++i)
				allDigits = s[i] >= '0' && s[i] <= '9';
			if (allDigits)
				maxN = std::max(maxN,
					static_cast<uint32_t>(std::stoul(s.substr(base.size() + 1))));
		}
	}
	char num[16];
	std::snprintf(num, sizeof(num), "%04u", maxN + 1);
	const std::string next =
		(dir.empty() ? fs::path(base + "." + num + ".tour")
			: dir / (base + "." + num + ".tour")).string();
	if (!SaveTourTo(next))
		return false;
	Log("Tour incrementally saved: " + next +
		" (" + std::to_string(m_historyCursor) + " commands).");
	return true;
}

bool EditorApp::SaveTourTo(const std::string& tp) {
	if (tp.empty() || !m_world)
		return false;
	{
		// Save-as may point at a directory that does not exist yet.
		std::error_code ec;
		const std::filesystem::path parent =
			std::filesystem::path(tp).parent_path();
		if (!parent.empty())
			std::filesystem::create_directories(parent, ec);
	}
	smvk::tour::Doc doc;
	doc.source = m_gltfPath;
	doc.fingerprint = smvk::tour::Fingerprint(m_gltfHostPath);
	// UUID pre-pass: nodes addressed by uuid or symbolic -p need a
	// file-stable identity recorded. Assign missing ones NOW into the
	// world scene (deterministic from here: already-assigned ids are
	// kept). Covers Delete targets AND symbolic parent targets (a
	// parent addressed by uuid must exist in the file) — named parents
	// resolve by name and need nothing.
	Scene& scene = m_world->scene;
	auto held = [&](const std::string& hex, uint32_t except) {
		for (const auto& [n, u] : scene.uuidForNode)
			if (u == hex && n != except)
				return true;
		return false;
	};
	auto ensureUuid = [&](uint32_t wnode) {
		if (wnode >= scene.hierarchy.size())
			return;
		// Named nodes ride names/paths in the symbolic -p slot — only
		// UNNAMED targets need a recorded uuid to stay addressable.
		if (!getNodeName(scene, int(wnode)).empty())
			return;
		if (scene.uuidForNode.contains(wnode))
			return;
		if (WorldToCluster(wnode) < 0)
			return; // editor-created: nothing to address
		// Maya setUuid parity: never hand out a uuid another live
		// node holds (regenerate; 128-bit collisions are ~impossible
		// but stale/duplicated tour lines must not steal identity).
		std::string hex;
		do {
			hex = smvk::tour::GenerateUuid();
		} while (held(hex, wnode));
		scene.uuidForNode[wnode] = hex;
	};
	for (size_t i : CoalescedSaveIndices()) {
		const EditCmd& c = m_history[i];
		if (c.verb == EditVerb::Delete) {
			const int cn = WorldToCluster(c.node);
			if (cn < 0)
				continue; // editor-created: dropped via live-state emission
			auto uit = scene.uuidForNode.find(c.node);
			if (uit != scene.uuidForNode.end()
				&& !held(uit->second, c.node))
				continue; // file-stable identity already in place
			ensureUuid(c.node);
			continue;
		}
		// Symbolic parents: unnamed targets need recorded uuids so
		// the -p slot can carry them (named ones ride names/paths).
		if (c.verb == EditVerb::Parent) {
			ensureUuid(c.node);
			ensureUuid(c.node2);
		}
		else if (c.verb == EditVerb::CreateLight
			|| c.verb == EditVerb::CreateTerrain
			|| c.verb == EditVerb::CreateTransform
			|| c.verb == EditVerb::CreateCVolume) {
			ensureUuid(c.node2);
		}
	}
	// Emit the whole identity map (cluster-backed, node-id order —
	// deterministic files): these feed the delete lines AND survive
	// reloads. Without this, a reload drops the identities and the
	// next save mints fresh ones (uuid churn: -uuid links would never
	// match twice).
	std::vector<uint32_t> uuidNodes;
	for (const auto& [wnode, hex] : scene.uuidForNode) {
		if (wnode >= scene.hierarchy.size())
			continue;
		if (WorldToCluster(wnode) < 0)
			continue; // editor-created: nothing to address
		uuidNodes.push_back(wnode);
	}
	std::sort(uuidNodes.begin(), uuidNodes.end());
	for (uint32_t wnode : uuidNodes) {
		smvk::tour::Command u{};
		u.verb = smvk::tour::Verb::SetUuid;
		u.node = uint32_t(WorldToCluster(wnode));
		u.uuid = scene.uuidForNode[wnode];
		u.name = getNodeName(scene, int(wnode));
		doc.commands.push_back(std::move(u));
	}
	// Save-time coalescing: only the surviving state per class lands
	// in the FILE (toggle pairs like hide/show/hide collapse to the
	// final command); the live stack, undo and the autosave keep
	// everything. Structural commands never drop (same classes as
	// CoalesceHistory).
	const std::vector<size_t> kept = CoalescedSaveIndices();
	for (size_t i : kept) {
		// Editor-created Delete targets (no cluster id: created lights,
		// terrain groups/tiles) never persist — their creates are
		// dropped by the live-state emission, which removes them from
		// the file. Skipped silently here: the unmapped-command log
		// below is for translation misses. Cluster-backed deletes map
		// through CmdToTour below (Verb::Delete).
		if (m_history[i].verb == EditVerb::Delete
			&& WorldToCluster(m_history[i].node) < 0)
			continue;
		// Placeholder visibility rides the reference line's `enabled`
		// bool — never the command log (purges legacy toggle lines).
		if (m_history[i].verb == EditVerb::SetVisibility
			&& IsRefPlaceholder(m_history[i].node))
			continue;
		smvk::tour::Command tc{};
		if (!CmdToTour(*this, m_history[i], tc)) {
			Log("tour: skipping unmapped command #" + std::to_string(i));
			continue;
		}
		doc.commands.push_back(std::move(tc));
	}
	// Orphans (unresolved at the last replay — e.g. edits on a replaced
	// or failed reference): re-emit VERBATIM so they resolve again when
	// their nodes come back. A reference round-trip must never erase
	// edits from the file.
	doc.commands.insert(doc.commands.end(),
		m_orphanTourCmds.begin(), m_orphanTourCmds.end());
	SaveOverridesToDoc(doc);
	SaveEnvToDoc(doc); // editor IBL / env nodes — never applied at cook
	SavePlacementToDoc(doc); // scatter graphs — editor-only, never cooked
	SaveImagesToDoc(doc); // image definitions — editor-only, never cooked
	doc.references = m_references; // scene references (loader binds them)
	doc.hasCook = true;
	// Cook SETTINGS persist; the DESTINATION does not: outPak is
	// machine/session state (entered per cook in the Cook panel),
	// never scene state — a checked-in .tour must not pin a local
	// output path.
	doc.cook.outPak.clear();
	doc.cook.meshopt = m_cook.meshopt;
	doc.cook.lods = m_cook.lods;
	doc.cook.lightmap = m_cook.lightmap;
	doc.cook.lmMetersPerTexel = m_cook.lmMetersPerTexel;
	doc.cook.lmLobes = m_cook.lmLobes;
	doc.cook.lmAtlasWidth = m_cook.lmAtlasWidth;
	doc.cook.lmRays = m_cook.lmRays;
	doc.cook.lmProxyRes = m_cook.lmProxyRes;
	doc.cook.lmExcludeSmall = m_cook.lmExcludeSmall;
	doc.cook.lmExcludeKw = m_cook.lmExcludeKw;
	doc.cook.sunAzimuth = m_cook.sunAzimuth;
	doc.cook.sunElevation = m_cook.sunElevation;
	doc.cook.sunIntensity = m_cook.sunIntensity;
	doc.cook.skyIntensity = m_cook.skyIntensity;
	std::string err;
	if (!smvk::tour::SaveTour(tp, doc, err)) {
		Log("tour save failed: " + err);
		return false;
	}
	m_tourPath = tp;
	// Explicit project-file writes (anything but the autosave slot)
	// clear the dirty star; the autosave path never does.
	if (tp != AutosavePath())
		m_tourDirty = false;
	return true;
}

bool EditorApp::TourToCmd(EditorApp& app, const smvk::tour::Command& tc, EditCmd& out) {
	out = EditCmd{};
	if (!app.m_world)
		return false;
	// Target resolution: generic verbs address cluster ids OR created-
	// terrain refs (kCreateTerrainRefBase | k, creation order — the same
	// scheme ApplyTourToCluster's resolveNode uses). Resolved once here;
	// every generic case below uses `node`. Create*/SetLightAttr/SetMatAttr
	// own their targeting and ignore it.
	auto resolveRef = [&](uint32_t raw, uint32_t& worldNode) -> bool {
		if ((raw & smvk::tour::kCreateTerrainRefBase) != 0) {
			const uint32_t k = raw & ~smvk::tour::kCreateTerrainRefBase;
			if (k >= app.m_tourCreatedTerrainNodes.size())
				return false;
			worldNode = app.m_tourCreatedTerrainNodes[k];
			return true;
		}
		const uint32_t w = app.ClusterToWorld(raw);
		if (w == UINT32_MAX || w >= app.m_world->scene.hierarchy.size())
			return false;
		worldNode = w;
		return true;
	};
	uint32_t node = UINT32_MAX;
	(void)resolveRef(tc.node, node); // UINT32_MAX on miss; needNode rejects
	auto needNode = [&]() -> bool {
		if (node == UINT32_MAX || node >= app.m_world->scene.hierarchy.size())
			return false;
		out.node = node;
		return true;
	};
	switch (tc.verb) {
	case smvk::tour::Verb::Rename:
		if (!needNode())
			return false;
		out.verb = EditVerb::Rename;
		out.afterText = tc.text;
		out.beforeText = getNodeName(app.m_world->scene, int(node));
		return true;
	case smvk::tour::Verb::SetTranslate:
		if (!needNode())
			return false;
		out.verb = EditVerb::Translate;
		out.afterVec = tc.vec;
		out.beforeMat = app.m_world->scene.localTransform[node];
		return true;
	case smvk::tour::Verb::SetRotate:
		if (!needNode())
			return false;
		out.verb = EditVerb::Rotate;
		out.afterVec = tc.vec;
		out.beforeMat = app.m_world->scene.localTransform[node];
		return true;
	case smvk::tour::Verb::SetScale:
		if (!needNode())
			return false;
		out.verb = EditVerb::Scale;
		out.afterVec = tc.vec;
		out.beforeMat = app.m_world->scene.localTransform[node];
		return true;
	case smvk::tour::Verb::AssignMaterial: {
		if (!needNode())
			return false;
		const int wm = app.ClusterMatToWorld(tc.material);
		if (wm < 0)
			return false;
		out.verb = EditVerb::AssignMaterial;
		out.material = uint32_t(wm);
		auto it = app.m_world->scene.materialForNode.find(node);
		out.beforeMaterial = (it != app.m_world->scene.materialForNode.end())
			? it->second : UINT32_MAX;
		return true;
	}
	case smvk::tour::Verb::SetMatAttr: {
		const int wm = app.ClusterMatToWorld(tc.material);
		if (wm < 0)
			return false;
		out.verb = EditVerb::SetMatAttr;
		out.matWorld = uint32_t(wm);
		out.attr = tc.text;
		out.vec4 = tc.vec4;
		out.number = tc.number;
		// "layers": cluster pool ids (file) -> world material ids.
		if (tc.text == "layers") {
			auto toWorld = [&](float poolId) -> float {
				if (poolId < 0.0f)
					return -1.0f;
				const int w = app.ClusterMatToWorld(uint32_t(poolId));
				return (w >= 0) ? float(w) : -1.0f;
			};
			out.vec4.x = toWorld(tc.vec4.x);
			out.vec4.y = toWorld(tc.vec4.y);
			out.vec4.z = toWorld(tc.vec4.z);
		}
		app.ReadSharedMatAttr(uint32_t(wm), tc.text, out.beforeVec4, out.beforeNum);
		return true;
	}
	case smvk::tour::Verb::SetNoLightmap:
		if (!needNode())
			return false;
		out.verb = EditVerb::SetNoLightmap;
		out.afterFlag = tc.flag;
		out.beforeFlag = node < app.m_world->scene.noLightmap.size() &&
			app.m_world->scene.noLightmap[node] != 0;
		return true;
	case smvk::tour::Verb::SetDitherCloseup:
		if (!needNode())
			return false;
		out.verb = EditVerb::SetDitherCloseup;
		out.afterFlag = tc.flag;
		out.beforeFlag = node < app.m_world->scene.ditherCloseup.size() &&
			app.m_world->scene.ditherCloseup[node] != 0;
		return true;
	case smvk::tour::Verb::SetVisibility:
		if (!needNode())
			return false;
		out.verb = EditVerb::SetVisibility;
		out.afterFlag = tc.flag;
		out.beforeFlag = node < app.m_world->scene.hidden.size() &&
			app.m_world->scene.hidden[node] != 0;
		return true;
	case smvk::tour::Verb::Parent: {
		if (!needNode())
			return false;
		// New parent: symbolic -p form (name/path/uuid) resolves
		// against the live world; numeric cluster id is the legacy
		// path (created-terrain refs included, as before).
		uint32_t wp = UINT32_MAX;
		if (!tc.parentRef.empty()) {
			if (!smvk::tour::ResolveParentTarget(app.m_world->scene,
					tc.parentRef, wp))
				return false;
		}
		else if (!resolveRef(tc.node2, wp)) {
			return false;
		}
		out.verb = EditVerb::Parent;
		out.node2 = wp;
		out.beforeParent = uint32_t(
			app.m_world->scene.hierarchy[node].parent);
		return true;
	}
	case smvk::tour::Verb::CreateTransform: {
		uint32_t wp = UINT32_MAX;
		if (!tc.parentRef.empty()) {
			if (!smvk::tour::ResolveParentTarget(app.m_world->scene,
					tc.parentRef, wp))
				return false;
		}
		else {
			wp = app.ClusterToWorld(tc.node);
			if (wp == UINT32_MAX
				|| wp >= app.m_world->scene.hierarchy.size())
				return false;
		}
		out.verb = EditVerb::CreateTransform;
		out.node = UINT32_MAX; // assigned at forward execution
		out.node2 = wp;        // parent (world)
		out.afterText = tc.text;
		out.afterVec = tc.vec;
		out.vec4 = glm::vec4(tc.vec4.x, tc.vec4.y, tc.vec4.z, 0.0f);
		out.svec = tc.svec;
		return true;
	}
	case smvk::tour::Verb::CreateNode: {
		// Defensive mapping for hand-built Docs (the parser lowers DAG
		// createNode lines to the legacy verbs, so file input never
		// lands here with a DAG type). System types are editor-managed:
		// kept as orphans by the caller (preserved verbatim).
		const smvk::tour::CreateNodeType* t =
			smvk::tour::FindCreateNodeType(tc.nodeType);
		if (!t || t->kind != smvk::tour::CreateNodeKind::Dag)
			return false;
		uint32_t wp = UINT32_MAX;
		if (!tc.parentRef.empty()) {
			if (!smvk::tour::ResolveParentTarget(app.m_world->scene,
					tc.parentRef, wp))
				return false;
		}
		else {
			wp = app.ClusterToWorld(tc.node);
			if (wp == UINT32_MAX
				|| wp >= app.m_world->scene.hierarchy.size())
				return false;
		}
		const std::string tn = t->name;
		if (tn == "transform") {
			out.verb = EditVerb::CreateTransform;
			out.node = UINT32_MAX;
			out.node2 = wp;
			out.afterText = tc.text;
			out.afterVec = tc.vec;
			out.vec4 = glm::vec4(tc.vec4.x, tc.vec4.y, tc.vec4.z, 0.0f);
			out.svec = tc.svec;
			return true;
		}
		if (tn == "cvolume") {
			out.verb = EditVerb::CreateCVolume;
			out.node = UINT32_MAX;
			out.node2 = wp;
			out.afterText = tc.text;
			out.afterVec = tc.vec;
			out.svec = tc.svec;
			out.cvolPriority = uint32_t(std::max(tc.vec4.x, 0.0f));
			out.cvolSpacing = std::max(tc.vec4.y, 1e-3f);
			out.cvolInsideFade = std::max(tc.vec4.z, 0.0f);
			out.cvolOutsideFade = std::max(tc.vec4.w, 0.0f);
			return true;
		}
		if (tn == "terrain") {
			out.verb = EditVerb::CreateTerrain;
			out.node = UINT32_MAX;
			out.beforeParent = wp;
			out.node2 = tc.node2;
			out.afterText = tc.text;
			out.afterVec = tc.vec;
			out.vec4 = tc.vec4;
			out.number = tc.number;
			const int wm = app.ClusterMatToWorld(tc.material);
			out.material = (wm >= 0) ? uint32_t(wm) : UINT32_MAX;
			return true;
		}
		// pointLight | spotLight.
		out.verb = EditVerb::CreateLight;
		out.node = UINT32_MAX;
		out.node2 = wp;
		out.afterText = tc.text;
		out.afterVec = tc.vec;
		out.lightType = (tc.vec4.x >= 1.5f) ? 2u : 1u;
		out.lightColor = glm::vec3(1.0f, 0.95f, 0.85f);
		out.lightIntensity = tc.vec4.y;
		out.lightRange = tc.vec4.z;
		out.lightInnerDeg = tc.vec4.w;
		out.lightOuterDeg = float(tc.number);
		out.lightStatic = tc.flag;
		out.lightShadow = tc.node2 != 0;
		return true;
	}
	case smvk::tour::Verb::Delete: {
		// uuid primary (file-stable identity across re-imports that
		// shift numeric ids), name secondary, numeric id fallback
		// (legacy lines + disambiguator for duplicated names).
		// Live-state capture happens at ExecCmd time (ExecDelete
		// records the links/entries from the live world), so the
		// replayed command is exactly undoable like a session delete.
		uint32_t target = UINT32_MAX;
		const uint32_t w = app.ClusterToWorld(tc.node);
		const uint32_t wid = (w != UINT32_MAX
			&& w < app.m_world->scene.hierarchy.size())
			? w : UINT32_MAX;
		if (!smvk::tour::ResolveDeleteTarget(app.m_world->scene,
				tc.uuid, tc.name, wid, target))
			return false;
		out.verb = EditVerb::Delete;
		out.node = target;
		return true;
	}
	case smvk::tour::Verb::CreateLight: {
		uint32_t wp = UINT32_MAX;
		if (!tc.parentRef.empty()) {
			if (!smvk::tour::ResolveParentTarget(app.m_world->scene,
					tc.parentRef, wp))
				return false;
		}
		else {
			wp = app.ClusterToWorld(tc.node);
			if (wp == UINT32_MAX
				|| wp >= app.m_world->scene.hierarchy.size())
				return false;
		}
		out.verb = EditVerb::CreateLight;
		out.node = UINT32_MAX; // assigned at forward execution
		out.node2 = wp;        // parent (world)
		out.afterText = tc.text;
		out.afterVec = tc.vec;
		out.lightType = (tc.vec4.x >= 1.5f) ? 2u : 1u;
		// Creation default (matches DraftCreateLight); a non-default
		// color always travels as its own SetLightAttr(color) command.
		out.lightColor = glm::vec3(1.0f, 0.95f, 0.85f);
		out.lightIntensity = tc.vec4.y;
		out.lightRange = tc.vec4.z;
		out.lightInnerDeg = tc.vec4.w;
		out.lightOuterDeg = float(tc.number);
		out.lightStatic = tc.flag;
		out.lightShadow = tc.node2 != 0;
		return true;
	}
	case smvk::tour::Verb::CreateCVolume: {
		uint32_t wp = UINT32_MAX;
		if (!tc.parentRef.empty()) {
			if (!smvk::tour::ResolveParentTarget(app.m_world->scene,
					tc.parentRef, wp))
				return false;
		}
		else {
			wp = app.ClusterToWorld(tc.node);
			if (wp == UINT32_MAX
				|| wp >= app.m_world->scene.hierarchy.size())
				return false;
		}
		out.verb = EditVerb::CreateCVolume;
		out.node = UINT32_MAX; // assigned at forward execution
		out.node2 = wp;        // parent (world)
		out.afterText = tc.text;
		out.afterVec = tc.vec;
		out.svec = tc.svec;
		out.cvolPriority = uint32_t(std::max(tc.vec4.x, 0.0f));
		out.cvolSpacing = std::max(tc.vec4.y, 1e-3f);
		out.cvolInsideFade = std::max(tc.vec4.z, 0.0f);
		out.cvolOutsideFade = std::max(tc.vec4.w, 0.0f);
		return true;
	}
	case smvk::tour::Verb::CreateTerrain: {
		uint32_t wp = UINT32_MAX;
		if (!tc.parentRef.empty()) {
			if (!smvk::tour::ResolveParentTarget(app.m_world->scene,
					tc.parentRef, wp))
				return false;
		}
		else {
			wp = app.ClusterToWorld(tc.node);
			if (wp == UINT32_MAX
				|| wp >= app.m_world->scene.hierarchy.size())
				return false;
		}
		out.verb = EditVerb::CreateTerrain;
		out.node = UINT32_MAX; // assigned at forward execution
		// Parent rides beforeParent (node2 is the tile split).
		out.beforeParent = wp;
		out.node2 = tc.node2;  // tilesX | (tilesZ << 16)
		out.afterText = tc.text;
		out.afterVec = tc.vec;
		out.vec4 = tc.vec4; // (size, res, height, freq)
		out.number = tc.number; // seed
		const int wm = app.ClusterMatToWorld(tc.material);
		out.material = (wm >= 0) ? uint32_t(wm) : UINT32_MAX;
		return true;
	}
	case smvk::tour::Verb::TerrainPaint: {
		const uint32_t k = tc.node & ~smvk::tour::kCreateTerrainRefBase;
		if ((tc.node & smvk::tour::kCreateTerrainRefBase) == 0
			|| k >= app.m_tourCreatedTerrainNodes.size())
			return false;
		out.verb = EditVerb::TerrainPaint;
		out.node = app.m_tourCreatedTerrainNodes[k]; // group world id
		out.terrainOp = tc.text;
		out.vec4 = tc.vec4;      // (worldX, worldZ, radius, target)
		out.number = tc.number;  // strength
		return true;
	}
	case smvk::tour::Verb::SetLightAttr: {
		uint32_t target = UINT32_MAX;
		if ((tc.node & smvk::tour::kCreateLightRefBase) != 0) {
			const uint32_t k = tc.node & ~smvk::tour::kCreateLightRefBase;
			if (k >= app.m_tourCreatedNodes.size())
				return false;
			target = app.m_tourCreatedNodes[k];
		} else {
			target = app.ClusterToWorld(tc.node);
		}
		if (target == UINT32_MAX
			|| target >= app.m_world->scene.hierarchy.size())
			return false;
		out.verb = EditVerb::SetLightAttr;
		out.node = target;
		out.lightAttr = tc.text;
		// Before-state from the live world (exact undo).
		if (!app.ReadNodeLightAttr(target, tc.text, out.beforeVec4,
				out.beforeNum, out.beforeFlag, out.beforeText))
			return false;
		if (tc.text == "color" || tc.text == "position"
			|| tc.text == "rotation") {
			out.vec4 = tc.vec4;
		} else if (tc.text == "name") {
			out.afterText = tc.name;
		} else if (tc.text == "static" || tc.text == "shadow") {
			out.afterFlag = tc.flag;
		} else {
			out.number = tc.number;
		}
		return true;
	}
	case smvk::tour::Verb::SetCvolAttr: {
		uint32_t target = UINT32_MAX;
		if ((tc.node & smvk::tour::kCreateCvolRefBase) != 0) {
			const uint32_t k = tc.node & ~smvk::tour::kCreateCvolRefBase;
			if (k >= app.m_tourCreatedCvolNodes.size())
				return false;
			target = app.m_tourCreatedCvolNodes[k];
		} else {
			target = app.ClusterToWorld(tc.node);
		}
		if (target == UINT32_MAX
			|| target >= app.m_world->scene.hierarchy.size())
			return false;
		if (!app.m_world->scene.cvolForNode.contains(target))
			return false; // not a cvolume: orphan, preserved verbatim
		out.verb = EditVerb::SetCvolAttr;
		out.node = target;
		out.cvolAttr = tc.text;
		// Before-state from the live world (exact undo).
		if (!app.ReadNodeCvolAttr(target, tc.text, out.beforeVec4,
				out.beforeNum, out.beforeText))
			return false;
		if (tc.text == "position" || tc.text == "rotation"
			|| tc.text == "size") {
			out.vec4 = tc.vec4;
		} else if (tc.text == "name") {
			out.afterText = tc.name;
		} else {
			out.number = tc.number;
		}
		return true;
	}
	}
	return false;
}

bool EditorApp::LoadTour() {
	const std::string tp = TourPath();
	if (tp.empty() || !m_world)
		return false;
	if (!std::filesystem::exists(tp))
		return true; // no tour yet: clean history (an autosave, if any,
	                 // is offered separately — never silently preferred)
	smvk::tour::Doc doc;
	std::string err;
	std::vector<std::string> warns;
	if (!smvk::tour::LoadTour(tp, doc, err, &warns)) {
		Log("tour load failed: " + err);
		return false;
	}
	for (auto& w : warns)
		Log(w);
	return ReplayTourDoc(doc, m_gltfPath);
}

bool EditorApp::ReplayTourDoc(smvk::tour::Doc& doc,
	const std::string& sourcePath) {
	if (!m_world)
		return false;
	if (!doc.fingerprint.empty()) {
		const std::string now = smvk::tour::Fingerprint(sourcePath);
		if (!now.empty() && now != doc.fingerprint)
			Log("tour: source changed since save (ids may have shifted)");
	}
	// Cook block restores the panel.
	if (doc.hasCook) {
		m_cook.outPak = doc.cook.outPak;
		m_cook.meshopt = doc.cook.meshopt;
		m_cook.lods = doc.cook.lods;
		m_cook.lightmap = doc.cook.lightmap;
		m_cook.lmMetersPerTexel = doc.cook.lmMetersPerTexel;
		m_cook.lmLobes = doc.cook.lmLobes;
		m_cook.lmAtlasWidth = doc.cook.lmAtlasWidth;
		m_cook.lmRays = doc.cook.lmRays;
		m_cook.lmProxyRes = doc.cook.lmProxyRes;
		m_cook.lmExcludeSmall = doc.cook.lmExcludeSmall;
		m_cook.lmExcludeKw = doc.cook.lmExcludeKw;
		m_cook.sunAzimuth = doc.cook.sunAzimuth;
		m_cook.sunElevation = doc.cook.sunElevation;
		m_cook.sunIntensity = doc.cook.sunIntensity;
		m_cook.skyIntensity = doc.cook.skyIntensity;
	}
	// Replay into the stack (fresh import underneath, so before-states are
	// exact and the whole tour stays undoable). Wide rules load AFTER the
	// direct commands so locals win over matching regex (more specific
	// wins); rules skip locally-overridden nodes when they apply.
	m_tourCreatedNodes.clear();
	m_tourCreatedCvolNodes.clear();
	m_replayingTour = true;
	ClearHistory();
	m_overrideRules.clear();
	m_nextOverrideId = 1;
	m_orphanTourCmds.clear();
	size_t applied = 0;
	for (auto& tc : doc.commands) {
		// `uuid` lines are structural persistence, not undoable edits:
		// apply the identity straight onto the live scene (no history
		// entry) so later deletes resolve through it. Maya setUuid
		// parity: rejected when another live node already holds it.
		if (tc.verb == smvk::tour::Verb::SetUuid) {
			const uint32_t target = ClusterToWorld(tc.node);
			if (target != UINT32_MAX
				&& target < m_world->scene.hierarchy.size()
				&& smvk::tour::SetNodeUuid(m_world->scene, target,
					tc.uuid)) {
				++applied;
			}
			else {
				m_orphanTourCmds.push_back(tc);
				Log("tour: keeping unresolved uuid line for later (" +
					smvk::tour::Describe(tc) + ").");
			}
			continue;
		}
		EditCmd c{};
		if (!TourToCmd(*this, tc, c)) {
			// Unresolvable (its node does not exist in this build):
			// KEEP the raw command as an orphan so the next save
			// re-emits it verbatim. A reload round-trip must never
			// erase edits.
			m_orphanTourCmds.push_back(tc);
			Log("tour: keeping unresolved command for later (" +
				smvk::tour::Describe(tc) + ").");
			continue;
		}
		ExecCmd(c, true);
		m_history.push_back(c);
		++applied;
	}
	m_replayingTour = false;
	m_historyCursor = m_history.size();
	Log("tour: applied " + std::to_string(applied) + "/"
		+ std::to_string(doc.commands.size()) + " commands"
		+ (m_orphanTourCmds.empty() ? std::string()
			: " (kept " + std::to_string(m_orphanTourCmds.size())
				+ " unresolved for later)."));
	m_tourDirty = false; // fresh replay matches its file
	LoadOverridesFromDoc(doc);
	LoadEnvFromDoc(doc); // editor IBL + env nodes (editorSet section)
	LoadPlacementFromDoc(doc); // scatter graphs (place.* editorSet lines)
	LoadImagesFromDoc(doc); // image definitions (img.* editorSet lines)
	// References were already bound at import (LoadGltfIntoWorld runs
	// ApplySceneReferences on this same file); mirror the list so the
	// editor state matches the parsed doc exactly.
	if (!doc.references.empty())
		m_references = doc.references;
	return true;
}

// ---------------------------------------------------------------------------
// Scene image definitions (Maya file nodes, editor-only): images live
// OUTSIDE the DAG like materials — pure editor state, persisted as
// img.* editorSettings (never cooked). A material using one mints the
// cluster texture resource at use time; until then these are authoring
// references (path + tiling/colorspace/filter).
// ---------------------------------------------------------------------------
void EditorApp::AddImageDef(const std::string& path) {
	if (path.empty())
		return;
	std::string stem = path;
	const size_t slash = stem.find_last_of("/\\");
	if (slash != std::string::npos)
		stem = stem.substr(slash + 1);
	const size_t dot = stem.find_last_of('.');
	if (dot != std::string::npos)
		stem = stem.substr(0, dot);
	ImageDef def;
	def.id = m_nextImageId++;
	def.name = stem;
	def.path = path;
	m_images.push_back(std::move(def));
	AutosaveTour();
	Log("Image added: '" + path + "'.");
}

void EditorApp::RemoveImageDefAt(size_t idx) {
	if (idx >= m_images.size())
		return;
	Log("Image removed: '" + m_images[idx].name + "'.");
	m_images.erase(m_images.begin() + ptrdiff_t(idx));
	AutosaveTour();
}

void EditorApp::SaveImagesToDoc(smvk::tour::Doc& doc) const {
	auto put = [&](const std::string& key, const std::string& val) {
		doc.editorSettings.emplace_back(key, val);
	};
	put("img.next", std::to_string(m_nextImageId));
	for (const ImageDef& img : m_images) {
		const std::string base = "img." + std::to_string(img.id);
		put(base + ".name", img.name);
		put(base + ".path", img.path);
		put(base + ".wrap", std::to_string(img.wrap));
		put(base + ".cs", std::to_string(img.colorspace));
		put(base + ".filter", std::to_string(img.filter));
	}
}

void EditorApp::LoadImagesFromDoc(const smvk::tour::Doc& doc) {
	auto u64 = [](const std::string& s, uint64_t fb) {
		try { return s.empty() ? fb : std::stoull(s); }
		catch (...) { return fb; }
	};
	auto toI = [](const std::string& s, int fb) {
		try { return s.empty() ? fb : std::stoi(s); }
		catch (...) { return fb; }
	};
	// Parse into (id, key, value) first, then fold per-id records.
	std::map<uint64_t, ImageDef> defs;
	uint64_t nextId = 1;
	for (const auto& [key, val] : doc.editorSettings) {
		if (key == "img.next") {
			nextId = u64(val, 1);
			continue;
		}
		if (key.rfind("img.", 0) != 0)
			continue;
		const size_t idEnd = key.find('.', 4);
		if (idEnd == std::string::npos)
			continue;
		const uint64_t id = u64(key.substr(4, idEnd - 4), 0);
		if (id == 0)
			continue;
		ImageDef& def = defs[id];
		def.id = id;
		const std::string attr = key.substr(idEnd + 1);
		if (attr == "name")
			def.name = val;
		else if (attr == "path")
			def.path = val;
		else if (attr == "wrap")
			def.wrap = toI(val, 0);
		else if (attr == "cs")
			def.colorspace = toI(val, 0);
		else if (attr == "filter")
			def.filter = toI(val, 0);
	}
	m_images.clear();
	for (auto& [id, def] : defs) {
		if (def.path.empty())
			continue; // half-written record: skip
		m_images.push_back(def);
	}
	m_nextImageId = std::max(nextId, m_images.empty() ? 1
		: m_images.back().id + 1);
}

std::string EditorApp::ResolveTourSource(const std::string& tourPath,
	const std::string& source) const {	namespace fs = std::filesystem;
	if (source.empty())
		return {};
	std::error_code ec;
	if (fs::is_regular_file(source, ec))
		return source;
	const fs::path cand = fs::path(tourPath).parent_path() / source;
	if (fs::is_regular_file(cand, ec))
		return cand.string();
	// VFS-style content-relative path ("models/..."): try the current
	// content root, then the working-dir content tree.
	const std::string root = ResolveVfsRoot();
	if (!root.empty() && root != ".") {
		const fs::path rooted = fs::path(root) / source;
		if (fs::is_regular_file(rooted, ec))
			return rooted.string();
	}
	return source; // let the loader report the failure
}

bool EditorApp::OpenTourFile(const std::string& path) {
	smvk::tour::Doc doc;
	std::string err;
	std::vector<std::string> warns;
	if (!smvk::tour::LoadTour(path, doc, err, &warns)) {
		Log("tour open failed: " + err);
		return false;
	}
	for (auto& w : warns)
		Log(w);
	// The tour names its source: load it when it differs from (or when
	// nothing is open). Replay lands on the fresh import underneath.
	if (!doc.source.empty()) {
		const std::string src = ResolveTourSource(path, doc.source);
		if (!src.empty() && src != m_gltfPath) {
			const bool ok = HasPakExtension(src)
				? LoadPakIntoWorld(src) : LoadGltfIntoWorld(src);
			if (!ok) {
				Log("tour open: failed to load source '" + src + "'");
				return false;
			}
		} else if (!m_world) {
			Log("tour open: no scene open and no loadable source.");
			return false;
		}
	} else if (!m_world) {
		// Sourceless tour (the `source` mechanism is deprecated —
		// geometry may come entirely from references): build an empty
		// world bound against THIS tour file, then replay below.
		if (!LoadGltfIntoWorld({}, path)) {
			Log("tour open: failed to build an empty world.");
			return false;
		}
	}
	m_tourPath = path; // the opened file becomes the save target
	if (!ReplayTourDoc(doc, doc.source.empty() ? m_gltfPath : doc.source))
		return false;
	Log("Tour opened: " + path);
	return true;
}

// Created terrain groups (procedural mesh + height painting).
//
// A created terrain is a GROUP world node plus tilesX*tilesZ tile child
// nodes, each with a StaticMesh upload in the primary cluster linked
// through world mesh locations — the outliner, inspector, gizmos and
// picking all pick it up with no special cases. The GLOBAL height grid
// (TerrainGroupData) is the authoritative state: painting edits it and
// rebuilds the touched tile meshes (world mesh indices stay stable);
// the placement Terrain domain samples it so scatter previews land on
// the PAINTED surface.
//
// Undo tombstones the GROUP (hidden + tile draw entries removed — the
// subtree stays hidden through IsVisibleNode's hierarchical walk);
// ids stay stable, matching the created-light precedent.
// Tour persistence: CreateTerrain carries the creation (CmdToTour emits
// the LIVE params); TerrainPaint carries every brush stamp (determinis-
// tic replay: same seed grid + same ordered stamps = same heights);
// generic verbs address the group via kCreateTerrainRefBase.
// ---------------------------------------------------------------------------
uint32_t EditorApp::TerrainGroupOf(uint32_t node) const {
	if (node == UINT32_MAX)
		return UINT32_MAX;
	if (m_terrainGroups.contains(node))
		return node;
	// Tile: resolve through the parent (tiles are direct children).
	if (m_world && node < m_world->scene.hierarchy.size()) {
		const int p = m_world->scene.hierarchy[node].parent;
		if (p >= 0 && m_terrainGroups.contains(uint32_t(p)))
			return uint32_t(p);
	}
	return UINT32_MAX;
}

bool EditorApp::RebuildTerrainTiles(uint32_t groupNode, uint32_t x0,
	uint32_t x1, uint32_t z0, uint32_t z1) {
	auto it = m_terrainGroups.find(groupNode);
	if (it == m_terrainGroups.end() || !m_world)
		return false;
	TerrainGroup& g = it->second;
	Scene& scene = m_world->scene;
	std::vector<uint32_t> touched;
	smvk::placement::TerrainBrushTouchedTiles(x0, x1, z0, z1,
		g.data.heights.width, g.data.tilesX, g.data.tilesZ, touched);
	bool ok = true;
	for (uint32_t packed : touched) {
		const uint32_t tx = packed & 0xFFFFu;
		const uint32_t tz = packed >> 16;
		const uint32_t tileNode = g.tileNodes[size_t(tz) * g.data.tilesX + tx];
		auto meshIt = scene.meshForNode.find(tileNode);
		if (meshIt == scene.meshForNode.end())
			continue;
		const uint32_t worldMeshIdx = meshIt->second;
		if (worldMeshIdx >= m_world->meshLocations.size())
			continue;
		smvk::StaticMeshUpload upload;
		if (!smvk::placement::BuildTerrainTileUpload(g.data, tx, tz, upload)) {
			Log("Terrain tile rebuild failed.");
			ok = false;
			continue;
		}
		const auto& loc = m_world->meshLocations[worldMeshIdx];
		smvk::Cluster* cl = loc.cluster ? loc.cluster
			: m_world->clusters[0].get();
		if (loc.cluster)
			loc.cluster->DestroyMesh(loc.handle);
		const smvk::ClusterStaticMeshHandle mh = cl->UploadStaticMesh(upload);
		if (!mh.valid()) {
			Log("Terrain tile upload failed.");
			ok = false;
			continue;
		}
		cl->RegisterMeshUri("terrain/" + std::to_string(groupNode)
			+ "_" + std::to_string(tx) + "_" + std::to_string(tz), mh);
		m_world->meshLocations[worldMeshIdx] = { cl, mh };
		// World mesh index unchanged: drawItems/flags stay valid.
	}
	return ok;
}

void EditorApp::ExecCreateTerrain(EditCmd& c) {
	if (!m_world || m_world->clusters.empty() || !m_world->clusters[0])
		return;
	Scene& scene = m_world->scene;
	// Parent rides beforeParent (node2 packs the tile split). The draft
	// resolves it pre-execution; undo/replay read it back untouched.
	uint32_t parentNode = c.beforeParent;
	if (parentNode == UINT32_MAX || parentNode >= scene.hierarchy.size())
		parentNode = DefaultCreateParent();
	const int level = (parentNode == UINT32_MAX
		|| parentNode >= scene.hierarchy.size()) ? 0
		: scene.hierarchy[parentNode].level + 1;
	const int group = addNode(scene,
		parentNode == UINT32_MAX || parentNode >= scene.hierarchy.size()
			? -1 : int(parentNode), level);
	if (group < 0)
		return;
	c.node = uint32_t(group);
	scene.localTransform[size_t(group)] =
		glm::translate(glm::mat4(1.0f), c.afterVec);
	const uint32_t nameId = uint32_t(scene.nodeNames.size());
	scene.nodeNames.push_back(c.afterText);
	scene.nameForNode[uint32_t(group)] = nameId;
	// Group state: global grid from the params; tiles as children.
	TerrainGroup& g = m_terrainGroups[uint32_t(group)];
	g.deleted = false; // (re)creation: alive even when redoing over a tombstone
	g.data.params.origin = glm::vec2(c.afterVec.x, c.afterVec.z);
	g.data.params.size = std::max(c.vec4.x, 1.0f);
	g.data.params.res = std::clamp(
		uint32_t(std::max(c.vec4.y, 2.0f)), 2u, 512u);
	g.data.params.height = std::max(c.vec4.z, 0.0f);
	g.data.params.freq = std::max(c.vec4.w, 1e-4f);
	g.data.params.seed = c.number < 0.0 ? 0ULL : uint64_t(c.number);
	g.data.tilesX = std::clamp(uint32_t(std::max(c.node2 & 0xFFFFu, 1u)),
		1u, smvk::placement::kTerrainMaxTiles);
	g.data.tilesZ = std::clamp(
		uint32_t(std::max((c.node2 >> 16) & 0xFFFFu, 1u)),
		1u, smvk::placement::kTerrainMaxTiles);
	g.data.heights = smvk::placement::BuildTerrainHeights(g.data.params);
	g.tileNodes.resize(size_t(g.data.tilesX) * g.data.tilesZ);
	const uint32_t material = (c.material != UINT32_MAX
		&& c.material < m_world->materialLocations.size())
		? c.material : EnsureDefaultMaterial();
	const float tileW = g.data.params.size / float(g.data.tilesX);
	const float tileH = g.data.params.size / float(g.data.tilesZ);
	smvk::Cluster* cl = m_world->clusters[0].get();
	// Tile children: node + mesh + world wiring + draw entry each.
	for (uint32_t tz = 0; tz < g.data.tilesZ; ++tz)
		for (uint32_t tx = 0; tx < g.data.tilesX; ++tx) {
			const int tileId = addNode(scene, group, level + 1);
			if (tileId < 0)
				return;
			const uint32_t t = uint32_t(tileId);
			scene.localTransform[size_t(t)] = glm::translate(
				glm::mat4(1.0f),
				glm::vec3(float(tx) * tileW, 0.0f, float(tz) * tileH));
			const uint32_t tName = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.afterText + "_" + std::to_string(tx)
				+ "_" + std::to_string(tz));
			scene.nameForNode[t] = tName;
			smvk::StaticMeshUpload upload;
			if (!smvk::placement::BuildTerrainTileUpload(g.data, tx, tz,
					upload)) {
				Log("Terrain tile build failed.");
				return;
			}
			const smvk::ClusterStaticMeshHandle mh = cl->UploadStaticMesh(upload);
			if (!mh.valid()) {
				Log("Terrain tile upload failed.");
				return;
			}
			cl->RegisterMeshUri("terrain/" + std::to_string(group)
				+ "_" + std::to_string(tx) + "_" + std::to_string(tz), mh);
			const uint32_t worldMeshIdx =
				uint32_t(m_world->meshLocations.size());
			m_world->meshLocations.push_back({ cl, mh });
			scene.meshForNode[t] = worldMeshIdx;
			scene.materialForNode[t] = material;
			if (m_world->flags.size() <= size_t(t))
				m_world->flags.resize(size_t(t) + 1, 0);
			m_world->flags[size_t(t)] = smvk::IteratorFlags(
				smvk::IteratorFlagBit_Renderable |
				smvk::IteratorFlagBit_StaticVertexFmt |
				smvk::IteratorFlagBit_Opaque |
				smvk::IteratorFlagBit_CastShadow);
			m_world->drawItems.push_back(smvk::World::RenderItem{
				t, worldMeshIdx, material,
				m_world->flags[size_t(t)] });
			m_world->itemFlags.push_back(m_world->flags[size_t(t)]);
			g.tileNodes[size_t(tz) * g.data.tilesX + tx] = t;
		}
	m_world->flagLists.clear();
	for (uint32_t t : g.tileNodes)
		RefreshNodeDrawState(t);
	if (m_replayingTour)
		m_tourCreatedTerrainNodes.push_back(uint32_t(group));
	markAsChanged(scene, group);
	recalculateGlobalTransforms(scene);
	m_selected = uint32_t(group);
	Log("Created terrain '" + c.afterText + "' (#" + std::to_string(group)
		+ ", " + std::to_string(g.data.tilesX) + "x"
		+ std::to_string(g.data.tilesZ) + " tiles).");
}

void EditorApp::ExecUncreateTerrain(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	auto it = m_terrainGroups.find(n);
	if (it == m_terrainGroups.end())
		return;
	// Tombstone the GROUP: hidden hides the subtree (IsVisibleNode walks
	// ancestors), and removing the tile draw entries stops draws/picks.
	// Ids stay stable (created-light precedent).
	MarkHidden(scene, int(n));
	for (uint32_t t : it->second.tileNodes) {
		scene.meshForNode.erase(t);
		scene.materialForNode.erase(t);
		for (size_t i = m_world->drawItems.size(); i-- > 0;)
			if (m_world->drawItems[i].node == t) {
				m_world->drawItems.erase(m_world->drawItems.begin()
					+ ptrdiff_t(i));
				m_world->itemFlags.erase(m_world->itemFlags.begin()
					+ ptrdiff_t(i));
				break;
			}
	}
	m_world->flagLists.clear();
	Log("Undid terrain creation (#" + std::to_string(n)
		+ "): hidden, ids kept stable (pool entries retained).");
}

void EditorApp::ExecTerrainPaint(EditCmd& c) {
	auto it = m_terrainGroups.find(c.node);
	if (it == m_terrainGroups.end() || it->second.deleted)
		return; // deleted group: its tiles don't draw — stamps are moot
	TerrainGroup& g = it->second;
	smvk::placement::TerrainBrushOp op;
	if (c.terrainOp == "raise")
		op = smvk::placement::TerrainBrushOp::Raise;
	else if (c.terrainOp == "lower")
		op = smvk::placement::TerrainBrushOp::Lower;
	else if (c.terrainOp == "flatten")
		op = smvk::placement::TerrainBrushOp::Flatten;
	else if (c.terrainOp == "smooth")
		op = smvk::placement::TerrainBrushOp::Smooth;
	else
		return;
	// Capture the before-patch once (first forward exec); redos reuse it.
	if (c.terrainBefore.empty()) {
		uint32_t x0, x1, z0, z1;
		smvk::placement::TerrainBrushBox(g.data, c.vec4.x, c.vec4.y,
			c.vec4.z, x0, x1, z0, z1);
		smvk::placement::TerrainCopyBox(g.data, x0, x1, z0, z1,
			c.terrainBefore, c.terrainBoxW, c.terrainBoxH);
		c.terrainBoxX0 = x0;
		c.terrainBoxZ0 = z0;
	}
	smvk::placement::TerrainBrushStamp(g.data, op, c.vec4.x, c.vec4.y,
		c.vec4.z, float(c.number), c.vec4.w);
	RebuildTerrainTiles(c.node, c.terrainBoxX0,
		c.terrainBoxX0 + c.terrainBoxW - 1, c.terrainBoxZ0,
		c.terrainBoxZ0 + c.terrainBoxH - 1);
	// Painted terrain changed: bound placement graphs re-preview.
	MarkPlacementDirty();
}

int EditorApp::AddTextureFromFile(const std::string& path) {
	if (!m_world || m_world->clusters.empty() || !m_world->clusters[0]
		|| !m_context)
		return -1;
	// Read the payload first (the cluster keeps the source bytes for
	// cook re-embeds; the URI IS the file path — a filepath ref).
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		Log("Material editor: cannot open texture '" + path + "'.");
		return -1;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	if (bytes.empty()) {
		Log("Material editor: texture file is empty: " + path);
		return -1;
	}
	smvk::Cluster* cl = m_world->clusters[0].get();
	auto holder = smvk::LoadKtxTextureMemory(*m_context, bytes.data(),
		bytes.size(), path);
	if (!holder.valid()) {
		Log("Material editor: KTX2 load failed: " + path);
		return -1;
	}
	const smvk::TextureHandle h = holder;
	cl->textures.create(std::move(holder));
	cl->RegisterTextureUri(path, h);
	cl->SetTextureSource(path, std::move(bytes));
	// Refresh pool->RHI-handle + bindless maps so the renderer can bind
	// the new texture this frame.
	RebuildTexHandleMap();
	Log("Material editor: added texture '" + path + "' (pool "
		+ std::to_string(h.index()) + ").");
	return int(h.index());
}

void EditorApp::ExecUnterrainPaint(EditCmd& c) {
	auto it = m_terrainGroups.find(c.node);
	if (it == m_terrainGroups.end() || c.terrainBefore.empty())
		return;
	TerrainGroup& g = it->second;
	smvk::placement::TerrainWriteBox(g.data, c.terrainBoxX0,
		c.terrainBoxX0 + c.terrainBoxW - 1, c.terrainBoxZ0,
		c.terrainBoxZ0 + c.terrainBoxH - 1, c.terrainBefore,
		c.terrainBoxW);
	RebuildTerrainTiles(c.node, c.terrainBoxX0,
		c.terrainBoxX0 + c.terrainBoxW - 1, c.terrainBoxZ0,
		c.terrainBoxZ0 + c.terrainBoxH - 1);
}

uint32_t EditorApp::EnsureDefaultMaterial() {
	if (!m_world || m_world->clusters.empty() || !m_world->clusters[0])
		return UINT32_MAX;
	if (m_defaultMatWorld != UINT32_MAX
		&& m_defaultMatWorld < m_world->materialLocations.size())
		return m_defaultMatWorld;
	smvk::Cluster* cl = m_world->clusters[0].get();
	// Reuse an existing pool entry when the URI is already registered
	// (e.g. a pak scene cooked from a tour that minted one).
	if (auto hit = cl->FindMaterialByUri("editor://default")) {
		const uint32_t worldIdx = uint32_t(m_world->materialLocations.size());
		m_world->materialLocations.push_back({ cl, *hit });
		m_defaultMatWorld = worldIdx;
		return worldIdx;
	}
	smvk::Material def{};
	def.values.params.y = 0.0f;  // metallic
	def.values.params.z = 0.9f; // roughness: soft default
	def.rebuildPipeline();
	const smvk::ClusterMaterialHandle h = cl->materials.create(std::move(def));
	cl->RegisterMaterialUri("editor://default", h);
	const uint32_t worldIdx = uint32_t(m_world->materialLocations.size());
	m_world->materialLocations.push_back({ cl, h });
	m_defaultMatWorld = worldIdx;
	return worldIdx;
}

bool EditorApp::PaintTerrainAt(const glm::vec3& origin, const glm::vec3& dir) {
	const uint32_t group = TerrainGroupOf(m_selected);
	if (group == UINT32_MAX)
		return false;
	auto it = m_terrainGroups.find(group);
	if (it == m_terrainGroups.end())
		return false;
	// Ray against the group's global grid (world space).
	float t = 0.0f;
	if (!smvk::placement::TerrainRaycast(it->second.data.heights,
			origin, dir, 5000.0f, t))
		return false;
	const glm::vec3 hit = origin + dir * t;
	static const char* kOps[] = { "raise", "lower", "flatten", "smooth" };
	EditCmd c{};
	c.verb = EditVerb::TerrainPaint;
	c.node = group;
	c.terrainOp = kOps[std::clamp(m_paintOp, 0, 3)];
	c.vec4 = glm::vec4(hit.x, hit.z, m_paintRadius, 0.0f);
	// Flatten pulls toward the height under the cursor (intuitive default).
	if (m_paintOp == 2)
		c.vec4.w = it->second.data.heights.sampleHeight(hit.x, hit.z);
	c.number = double(m_paintStrength);
	Do(c);
	return true;
}

} // namespace editor
