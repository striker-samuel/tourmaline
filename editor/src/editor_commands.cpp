// Editor CRUD command stack: execute/do/undo/redo + .tour persistence.
//
// Live commands use WORLD-space ids; the .tour file uses CLUSTER-space ids
// (import-deterministic). Translation happens only at the save/load
// boundary (CmdToTour / TourToCmd).

#include "editor_app.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <Engine/Asset/tour.h>
#include <Engine/Flintbase/flintbase_backend.h>
#include <Engine/Scene/scene.h>
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
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateLight)
			continue;
		if (h.node == worldNode)
			return count;
		++count;
	}
	return -1;
}

std::string EditorApp::UniqueTerrainName() const {
	const char* base = "Terrain";
	int n = 1;
	if (m_world)
		for (size_t i = 0; i < m_world->scene.hierarchy.size(); ++i)
			if (m_world->scene.meshForNode.contains(i)) {
				const smvk::StaticMesh* m = GetMesh(i);
				if (m && m->geometry.vertexCount > 0) {
					const auto& geo = m->geometry;
					if (geo.aabbMax.x - geo.aabbMin.x > geo.aabbMax.y - geo.aabbMin.y * 2.0f)
						++n;
				}
			}
	return std::string(base) + "_" + std::to_string(n);
}

int EditorApp::CreatedTerrainIndex(uint32_t worldNode) const {
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateTerrain)
			continue;
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
	const uint32_t sel = m_selected;
	c.node2 = (sel != UINT32_MAX && sel < m_world->scene.hierarchy.size())
		? sel : DefaultCreateParent();
	c.afterText = d.name;
	c.afterVec = d.origin;
	c.vec4 = glm::vec4(d.size, float(d.res), d.height, d.freq);
	c.number = double(d.seed);
	c.material = d.material;
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
	SaveTour();
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
	SaveTour();
}

bool EditorApp::Undo() {
	if (m_historyCursor == 0)
		return false;
	--m_historyCursor;
	ExecCmd(m_history[m_historyCursor], false);
	ReassertWideOverrides(m_history[m_historyCursor].node);
	SaveTour();
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
	SaveTour();
	return true;
}

void EditorApp::ClearHistory() {
	m_history.clear();
	m_historyCursor = 0;
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
		if (!needNode())
			return false;
		out.verb = (c.verb == EditVerb::Translate)
			? smvk::tour::Verb::SetTranslate : smvk::tour::Verb::SetRotate;
		out.vec = c.afterVec;
		return true;
	}
	case EditVerb::Scale:
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::SetScale;
		out.vec = c.afterVec;
		return true;
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
		out.verb = smvk::tour::Verb::CreateLight;
		out.node = uint32_t(pp);
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
		// Emit the LIVE create state: the node may have moved and its
		// params may have been attr-edited after creation (those later
		// Material/Transform/etc commands re-emit harmlessly).
		const uint32_t created = c.node;
		if (created == UINT32_MAX
			|| created >= app.m_world->scene.hierarchy.size())
			return false;
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
		out.verb = smvk::tour::Verb::CreateTerrain;
		out.node = uint32_t(pp);
		out.text = c.afterText;
		out.vec = t;
		out.vec4 = c.vec4; // (size, res, height, freq)
		out.number = c.number; // seed
		out.material = c.material;
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
	return SaveTourTo(TourTarget());
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
	for (size_t i = 0; i < m_historyCursor; ++i) {
		smvk::tour::Command tc{};
		if (!CmdToTour(*this, m_history[i], tc)) {
			Log("tour: skipping unmapped command #" + std::to_string(i));
			continue;
		}
		doc.commands.push_back(std::move(tc));
	}
	SaveOverridesToDoc(doc);
	SaveEnvToDoc(doc); // editor IBL / env nodes — never applied at cook
	SavePlacementToDoc(doc); // scatter graphs — editor-only, never cooked
	doc.references = m_references; // scene references (loader binds them)
	doc.hasCook = true;
	doc.cook.outPak = m_cook.outPak;
	doc.cook.meshopt = m_cook.meshopt;
	doc.cook.lods = m_cook.lods;
	doc.cook.lightmap = m_cook.lightmap;
	doc.cook.lmMetersPerTexel = m_cook.lmMetersPerTexel;
	doc.cook.lmLobes = m_cook.lmLobes;
	doc.cook.lmAtlasWidth = m_cook.lmAtlasWidth;
	doc.cook.lmRays = m_cook.lmRays;
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
	return true;
}

bool EditorApp::TourToCmd(EditorApp& app, const smvk::tour::Command& tc, EditCmd& out) {
	out = EditCmd{};
	const uint32_t wn = app.ClusterToWorld(tc.node);
	auto needNode = [&](uint32_t n = wn) -> bool {
		if ((n & smvk::tour::kCreateTerrainRefBase) != 0) {
			const uint32_t k = n & ~smvk::tour::kCreateTerrainRefBase;
			if (k >= app.m_tourCreatedTerrainNodes.size())
				return false;
			out.node = app.m_tourCreatedTerrainNodes[k];
			return true;
		}
		if ((n & smvk::tour::kCreateLightRefBase) != 0) {
			const uint32_t k = n & ~smvk::tour::kCreateLightRefBase;
			if (k >= app.m_tourCreatedNodes.size())
				return false;
			out.node = app.m_tourCreatedNodes[k];
			return true;
		}
		if (n == UINT32_MAX || n >= app.m_world->scene.hierarchy.size())
			return false;
		out.node = n;
		return true;
	};
	auto needNodeWithTerrain = [&](uint32_t n) {
		if ((n & smvk::tour::kCreateTerrainRefBase) != 0) {
			const uint32_t k = n & ~smvk::tour::kCreateTerrainRefBase;
			if (k >= app.m_tourCreatedTerrainNodes.size())
				return false;
			out.node = app.m_tourCreatedTerrainNodes[k];
			return true;
		}
		if (n == UINT32_MAX || n >= app.m_world->scene.hierarchy.size())
			return false;
		out.node = n;
		return true;
	};
	switch (tc.verb) {
	case smvk::tour::Verb::Rename:
		if (!needNode())
			return false;
		out.verb = EditVerb::Rename;
		out.afterText = tc.text;
		out.beforeText = getNodeName(app.m_world->scene, int(wn));
		return true;
	case smvk::tour::Verb::SetTranslate:
		if (!needNode())
			return false;
		out.verb = EditVerb::Translate;
		out.afterVec = tc.vec;
		out.beforeMat = app.m_world->scene.localTransform[wn];
		return true;
	case smvk::tour::Verb::SetRotate:
		if (!needNode())
			return false;
		out.verb = EditVerb::Rotate;
		out.afterVec = tc.vec;
		out.beforeMat = app.m_world->scene.localTransform[wn];
		return true;
	case smvk::tour::Verb::SetScale:
		if (!needNode())
			return false;
		out.verb = EditVerb::Scale;
		out.afterVec = tc.vec;
		out.beforeMat = app.m_world->scene.localTransform[wn];
		return true;
	case smvk::tour::Verb::AssignMaterial: {
		if (!needNode())
			return false;
		const int wm = app.ClusterMatToWorld(tc.material);
		if (wm < 0)
			return false;
		out.verb = EditVerb::AssignMaterial;
		out.material = uint32_t(wm);
		auto it = app.m_world->scene.materialForNode.find(wn);
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
		out.beforeFlag = wn < app.m_world->scene.noLightmap.size() &&
			app.m_world->scene.noLightmap[wn] != 0;
		return true;
	case smvk::tour::Verb::SetDitherCloseup:
		if (!needNode())
			return false;
		out.verb = EditVerb::SetDitherCloseup;
		out.afterFlag = tc.flag;
		out.beforeFlag = wn < app.m_world->scene.ditherCloseup.size() &&
			app.m_world->scene.ditherCloseup[wn] != 0;
		return true;
	case smvk::tour::Verb::SetVisibility:
		if (!needNode())
			return false;
		out.verb = EditVerb::SetVisibility;
		out.afterFlag = tc.flag;
		out.beforeFlag = wn < app.m_world->scene.hidden.size() &&
			app.m_world->scene.hidden[wn] != 0;
		return true;
	case smvk::tour::Verb::Parent: {
		if (!needNode())
			return false;
		const uint32_t wp = app.ClusterToWorld(tc.node2);
		if (wp == UINT32_MAX || wp >= app.m_world->scene.hierarchy.size())
			return false;
		out.verb = EditVerb::Parent;
		out.node2 = wp;
		out.beforeParent = uint32_t(
			app.m_world->scene.hierarchy[wn].parent);
		return true;
	}
	case smvk::tour::Verb::CreateLight: {
		const uint32_t wp = app.ClusterToWorld(tc.node);
		if (wp == UINT32_MAX || wp >= app.m_world->scene.hierarchy.size())
			return false;
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
	case smvk::tour::Verb::CreateTerrain: {
		const uint32_t wp = app.ClusterToWorld(tc.node);
		if (wp == UINT32_MAX || wp >= app.m_world->scene.hierarchy.size())
			return false;
		out.verb = EditVerb::CreateTerrain;
		out.node = UINT32_MAX; // assigned at forward execution
		out.node2 = wp;        // parent (world)
		out.afterText = tc.text;
		out.afterVec = tc.vec;
		out.vec4 = tc.vec4; // (size, res, height, freq)
		out.number = tc.number; // seed
		out.material = tc.material;
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
	}
	return false;
}

bool EditorApp::LoadTour() {
	const std::string tp = TourPath();
	if (tp.empty() || !m_world)
		return false;
	if (!std::filesystem::exists(tp))
		return true; // no tour yet: clean history
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
	m_replayingTour = true;
	ClearHistory();
	m_overrideRules.clear();
	m_nextOverrideId = 1;
	size_t applied = 0;
	for (auto& tc : doc.commands) {
		EditCmd c{};
		if (!TourToCmd(*this, tc, c)) {
			Log("tour: skipping unmapped command (" +
				smvk::tour::Describe(tc) + ")");
			continue;
		}
		ExecCmd(c, true);
		m_history.push_back(c);
		++applied;
	}
	m_replayingTour = false;
	m_historyCursor = m_history.size();
	Log("tour: applied " + std::to_string(applied) + "/" +
		std::to_string(doc.commands.size()) + " commands.");
	LoadOverridesFromDoc(doc);
	LoadEnvFromDoc(doc); // editor IBL + env nodes (editorSet section)
	LoadPlacementFromDoc(doc); // scatter graphs (place.* editorSet lines)
	// References were already bound at import (LoadGltfIntoWorld runs
	// ApplySceneReferences on this same file); mirror the list so the
	// editor state matches the parsed doc exactly.
	if (!doc.references.empty())
		m_references = doc.references;
	return true;
}

std::string EditorApp::ResolveTourSource(const std::string& tourPath,
	const std::string& source) const {
	namespace fs = std::filesystem;
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
		Log("tour open: tour has no source and no scene is open.");
		return false;
	}
	m_tourPath = path; // the opened file becomes the save target
	if (!ReplayTourDoc(doc, doc.source.empty() ? m_gltfPath : doc.source))
		return false;
	Log("Tour opened: " + path);
	return true;
}

// Created terrain (procedural mesh).
//
// A created terrain is a WORLD node appended to the live scene plus a
// StaticMesh upload in the primary cluster, linked through world mesh
// locations — so the outliner, inspector, gizmos and the cooked-mesh
// census all pick it up with no special cases. Undo removes the node
// when it is still the childless tail (ids stay stable); otherwise the
// terrain is tombstoned (hidden + detached) for the same reason.
// Tour persistence: CreateTerrain carries the creation (CmdToTour emits
// the LIVE params, so post-creation edits ride along); generic verbs
// (Rename/SetTranslate/.../Parent) address later creates via
// kCreateTerrainRefBase (creation order).
// ---------------------------------------------------------------------------
void EditorApp::ExecCreateTerrain(EditCmd& c) {
	if (!m_world || m_world->clusters.empty() || !m_world->clusters[0])
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
	scene.localTransform[size_t(created)] = glm::translate(glm::mat4(1.0f), c.afterVec);
	// Name (the nodeNames pool is grow-only, like Rename).
	const uint32_t nameId = uint32_t(scene.nodeNames.size());
	scene.nodeNames.push_back(c.afterText);
	scene.nameForNode[uint32_t(created)] = nameId;
	// Build and upload the terrain mesh.
	TerrainParams tp{};
	tp.origin = glm::vec2(0.0f, 0.0f);
	tp.size = std::max(c.vec4.x, 1.0f);
	tp.res = std::clamp(uint32_t(std::max(c.vec4.y, 2.0f)), 2u, 512u);
	tp.height = std::max(c.vec4.z, 0.0f);
	tp.freq = std::max(c.vec4.w, 1e-4f);
	tp.seed = c.number < 0.0 ? 0ULL : uint64_t(c.number);
	StaticMeshUpload upload;
	if (!BuildTerrainUpload(tp, upload)) {
		Log("Terrain mesh build failed.");
		return;
	}
	const ClusterStaticMeshHandle mh = m_world->clusters[0]->UploadStaticMesh(upload);
	if (!mh.valid()) {
		Log("Terrain mesh upload failed.");
		return;
	}
	cluster.RegisterMeshUri(
		std::string("terrain/") + c.afterText, mh);
	scene.meshForNode[uint32_t(created)] = mh.index();
	scene.materialForNode[uint32_t(created)] = c.material;
	if (m_replayingTour)
		m_tourCreatedTerrainNodes.push_back(uint32_t(created));
	markAsChanged(scene, created);
	recalculateGlobalTransforms(scene);
	m_selected = uint32_t(created);
	Log("Created terrain '" + c.afterText + "' (#" + std::to_string(created) + ").");
}

void EditorApp::ExecUncreateTerrain(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	const bool tail = (n + 1 == uint32_t(scene.hierarchy.size()));
	const bool childless = scene.hierarchy[n].firstChild < 0;
	auto meshIt = scene.meshForNode.find(n);
	const bool meshTail = meshIt != scene.meshForNode.end()
		&& meshIt->second + 1 == m_world->clusters[0]->staticMeshes.size();
	if (tail && childless) {
		// Physical removal: pop every vector addNode grew (flag arrays
		// only when they are exactly node-sized — empty/mismatched
		// means "default", which needs no entry).
		if (meshIt != scene.meshForNode.end()) {
			if (meshTail) {
				const auto& mh = m_world->clusters[0]->staticMeshes.get(
					smvk::ClusterStaticMeshHandle(meshIt->second));
				if (mh.valid())
					m_world->clusters[0]->DestroyMesh(
						smvk::ClusterStaticMeshHandle(meshIt->second));
			}
		}
		scene.meshForNode.erase(n);
		scene.materialForNode.erase(n);
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
		Log("Undid terrain creation (#" + std::to_string(n) + ").");
	} else {
		// Non-tail (or parented) node: keep ids stable — hide the node
		// and detach the mesh instead of shifting the world.
		MarkHidden(scene, int(n));
		scene.meshForNode.erase(n);
		scene.materialForNode.erase(n);
		Log("Undid terrain creation (#" + std::to_string(n) +
		 "): hidden, ids kept stable (pool entry retained).");
	}
}

} // namespace editor
