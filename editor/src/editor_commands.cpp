// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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
	else if (attr == "contactShadows")
		num = (smvk::MaterialFlagsFromWord(mat->values.emissiveFactor.w)
			& smvk::kMaterialFlagReceiveContactShadows) != 0 ? 1.0 : 0.0;
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
		// w carries the per-material flag bits (mesh.h) - preserve them.
		mat->values.emissiveFactor = glm::vec4(v4.x, v4.y, v4.z,
			mat->values.emissiveFactor.w);
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
	else if (attr == "contactShadows")
		mat->values.emissiveFactor.w = smvk::MaterialFlagsToWord(
			(num != 0.0) ? smvk::kMaterialFlagReceiveContactShadows : 0u);
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
		SetNodeNameIndex(scene, c.node, id);
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
		SetMaterial(scene, c.node, forward ? c.material : c.beforeMaterial);
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
	case EditVerb::SetLightmap: {
		const bool v = forward ? c.afterFlag : c.beforeFlag;
		if (v)
			MarkLightmap(scene, int(c.node));
		else
			ClearLightmap(scene, int(c.node));
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
	case EditVerb::SetCastsVolumetricShadow: {
		const bool v = forward ? c.afterFlag : c.beforeFlag;
		if (v)
			MarkCastsVolumetricShadow(scene, int(c.node));
		else
			ClearCastsVolumetricShadow(scene, int(c.node));
		// The flag feeds the world's item-flag classification (the
		// CastVolumetricShadow iterator bit) - refresh the draw state
		// so the live renderer's caster walk sees the flip.
		RefreshNodeDrawState(c.node);
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
	case EditVerb::SetStatic: {
		// Whole-tree static mark (Scene::staticRoot): frozen transforms
		// after the initial build + mega-mesh collapse eligibility.
		// Mark/Clear rebuild the derived staticNode cache.
		const bool v = forward ? c.afterFlag : c.beforeFlag;
		if (v)
			MarkSubtreeStatic(scene, int(c.node));
		else
			ClearSubtreeStatic(scene, int(c.node));
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
	case EditVerb::CreateGiVolume: {
		if (forward)
			ExecCreateGiVolume(c);
		else
			ExecUncreateGiVolume(c);
		break;
	}
	case EditVerb::SetGiVolumeAttr: {
		const std::string& attr = c.giVolumeAttr;
		if (forward)
			WriteNodeGiVolumeAttr(c.node, attr, c.vec4, c.number,
				c.afterText);
		else
			WriteNodeGiVolumeAttr(c.node, attr, c.beforeVec4, c.beforeNum,
				c.beforeText);
		break;
	}
	case EditVerb::CreateCubemap: {
		if (forward)
			ExecCreateCubemap(c);
		else
			ExecUncreateCubemap(c);
		break;
	}
	case EditVerb::SetCubemapAttr: {
		const std::string& attr = c.cubemapAttr;
		if (forward)
			WriteNodeCubemapAttr(c.node, attr, c.vec4, c.number,
				c.afterText);
		else
			WriteNodeCubemapAttr(c.node, attr, c.beforeVec4,
				c.beforeNum, c.beforeText);
		break;
	}
	case EditVerb::CreateCamera: {
		if (forward)
			ExecCreateCamera(c);
		else
			ExecUncreateCamera(c);
		break;
	}
	case EditVerb::SetCamAttr: {
		const std::string& attr = c.camAttr;
		if (forward)
			WriteNodeCameraAttr(c.node, attr, c.vec4, c.number,
				c.afterText);
		else
			WriteNodeCameraAttr(c.node, attr, c.beforeVec4,
				c.beforeNum, c.beforeText);
		break;
	}
	case EditVerb::CreateExportSettings: {
		if (forward)
			ExecCreateExportSettings(c);
		else
			ExecUncreateExportSettings(c);
		break;
	}
	case EditVerb::SetExportAttr: {
		const std::string& attr = c.exportAttr;
		if (forward)
			WriteNodeExportAttr(c.node, attr, c.number, c.afterFlag,
				c.afterText);
		else
			WriteNodeExportAttr(c.node, attr, c.beforeNum, c.beforeFlag,
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
		for (const LightRow& row : m_world->scene.tables.lights) {
			const ::SceneLight* L = FindNodeLight(row.node, nullptr);
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
	// Non-EMITTING creates are skipped too (CreateCmdEmitsInTour): the
	// file's -ref space is the creates that actually land in the file.
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateLight)
			continue;
		if (!m_world || FindLight(m_world->scene, h.node) == nullptr)
			continue; // deleted (tombstoned): not in the file's ref space
		if (!CreateCmdEmitsInTour(*this, h))
			continue; // emission dropped (parent unresolvable)
		if (h.node == worldNode)
			return count;
		++count;
	}
	return -1;
}

int EditorApp::CreatedGiVolumeIndex(uint32_t worldNode) const {
	// Same contract as CreatedLightIndex, for the GI-volume family:
	// alive + emitting creates only (a deleted GI volume's CreateGiVolume
	// is dropped from the tour, so the file's -ref space spans the
	// survivors — save-time indices and replay-time
	// m_tourCreatedGiVolumeNodes match).
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateGiVolume)
			continue;
		if (!m_world || FindGiVolume(m_world->scene, h.node) == nullptr)
			continue; // deleted (tombstoned): not in the file's ref space
		if (!CreateCmdEmitsInTour(*this, h))
			continue; // emission dropped (parent unresolvable)
		if (h.node == worldNode)
			return count;
		++count;
	}
	return -1;
}

std::string EditorApp::UniqueGiVolumeName() const {
	// Next free "GIVolume_N" over live node names (1-based).
	int n = 1;
	if (m_world) {
		for (;;) {
			const std::string want = "GIVolume_" + std::to_string(n);
			bool taken = false;
			for (size_t i = 0; i < m_world->scene.hierarchy.size() && !taken; ++i)
				taken = getNodeName(m_world->scene, int(i)) == want;
			if (!taken)
				break;
			++n;
		}
	}
	return "GIVolume_" + std::to_string(n);
}

int EditorApp::CreatedCubemapIndex(uint32_t worldNode) const {
	// Same contract as CreatedGiVolumeIndex, for the cubemap-capture family
	// (alive + emitting creates only).
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateCubemap)
			continue;
		if (!m_world || FindCubemap(m_world->scene, h.node) == nullptr)
			continue; // deleted (tombstoned): not in the file's ref space
		if (!CreateCmdEmitsInTour(*this, h))
			continue; // emission dropped (parent unresolvable)
		if (h.node == worldNode)
			return count;
		++count;
	}
	return -1;
}

std::string EditorApp::UniqueCubemapName() const {
	// Next free "Cube_N" over live node names (1-based).
	int n = 1;
	if (m_world) {
		for (;;) {
			const std::string want = "Cube_" + std::to_string(n);
			bool taken = false;
			for (size_t i = 0; i < m_world->scene.hierarchy.size() && !taken; ++i)
				taken = getNodeName(m_world->scene, int(i)) == want;
			if (!taken)
				break;
			++n;
		}
	}
	return "Cube_" + std::to_string(n);
}

int EditorApp::CreatedCameraIndex(uint32_t worldNode) const {
	// Same contract as CreatedLightIndex, for the camera family: alive
	// + emitting creates only (a deleted camera's CreateCamera is
	// dropped from the tour, so the file's -ref space spans the
	// survivors).
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateCamera)
			continue;
		if (!m_world || FindCamera(m_world->scene, h.node) == nullptr)
			continue; // deleted (tombstoned): not in the file's ref space
		if (!CreateCmdEmitsInTour(*this, h))
			continue; // emission dropped (parent unresolvable)
		if (h.node == worldNode)
			return count;
		++count;
	}
	return -1;
}

std::string EditorApp::UniqueCameraName() const {
	// Next free "Camera_N" over live node names (1-based).
	int n = 1;
	if (m_world) {
		for (;;) {
			const std::string want = "Camera_" + std::to_string(n);
			bool taken = false;
			for (size_t i = 0; i < m_world->scene.hierarchy.size() && !taken; ++i)
				taken = getNodeName(m_world->scene, int(i)) == want;
			if (!taken)
				break;
			++n;
		}
	}
	return "Camera_" + std::to_string(n);
}

std::string EditorApp::UniqueLocatorName() const {
	// Next free "Locator_N" over live node names (1-based).
	int n = 1;
	if (m_world) {
		for (;;) {
			const std::string want = "Locator_" + std::to_string(n);
			bool taken = false;
			for (size_t i = 0; i < m_world->scene.hierarchy.size() && !taken; ++i)
				taken = getNodeName(m_world->scene, int(i)) == want;
			if (!taken)
				break;
			++n;
		}
	}
	return "Locator_" + std::to_string(n);
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

std::string EditorApp::UniqueCopyName(const std::string& base) const {
	// Duplicate naming (Edit > Duplicate): keep the original's prefix
	// and find the first free "_N" index, so duplicates stay grouped in
	// the outliner ("HeroCam_1" -> "HeroCam_2"; "HeroCam" -> "HeroCam_1").
	// A trailing "_<digits>" is treated as an index; anything else keeps
	// the whole name as the prefix.
	if (!m_world)
		return base;
	std::string prefix = base;
	size_t us = base.rfind('_');
	if (us != std::string::npos && us + 1 < base.size()) {
		bool digits = true;
		for (size_t d = us + 1; d < base.size(); ++d)
			if (!std::isdigit(static_cast<unsigned char>(base[d]))) {
				digits = false;
				break;
			}
		if (digits)
			prefix = base.substr(0, us);
	}
	for (int n = 1;; ++n) {
		const std::string want = prefix + "_" + std::to_string(n);
		bool taken = false;
		for (size_t i = 0; i < m_world->scene.hierarchy.size() && !taken; ++i)
			taken = getNodeName(m_world->scene, int(i)) == want;
		if (!taken)
			return want;
	}
}

int EditorApp::CreatedTerrainIndex(uint32_t worldNode) const {
	// Same contract as CreatedLightIndex: alive + emitting creates
	// only (deleted groups' CreateTerrain is dropped from the tour, so
	// the file's TerrainPaint -ref space spans the survivors).
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateTerrain)
			continue;
		auto it = m_terrainGroups.find(h.node);
		if (it == m_terrainGroups.end() || it->second.deleted)
			continue; // deleted (tombstoned): not in the file's ref space
		if (!CreateCmdEmitsInTour(*this, h))
			continue; // emission dropped (parent unresolvable)
		if (h.node == worldNode)
			return count;
		++count;
	}
	return -1;
}

bool EditorApp::CreateCmdEmitsInTour(const EditorApp& app, const EditCmd& h) {
	// Mirrors CmdToTour's per-family drop conditions EXACTLY (alive
	// link + not deleted + parent slot resolvable) so the family -ref
	// index spaces stay identical to the file's create sets. Any
	// divergence re-produces dangling "setXAttr -ref k" lines (the
	// "ref out of range" cook failures). The parent slot is the LIVE
	// parent (see CreateEmitParent): reparents ride the create line
	// itself, so the check must follow the live hierarchy, not the
	// recorded creation-time parent.
	if (!app.m_world)
		return false;
	const Scene& scene = app.m_world->scene;
	const uint32_t created = h.node;
	if (created == UINT32_MAX || created >= scene.hierarchy.size())
		return false;
	if (app.m_deletedNodes.contains(created))
		return false;
	switch (h.verb) {
	case EditVerb::CreateLight: {
		if (!app.FindNodeLight(created, nullptr))
			return false; // detached (tombstoned)
		break;
	}
	case EditVerb::CreateGiVolume: {
		if (FindGiVolume(scene, created) == nullptr)
			return false; // detached (tombstoned)
		break;
	}
	case EditVerb::CreateCubemap: {
		if (FindCubemap(scene, created) == nullptr)
			return false; // detached (tombstoned)
		break;
	}
	case EditVerb::CreateCamera: {
		if (!app.FindNodeCamera(created))
			return false; // detached (tombstoned)
		break;
	}
	case EditVerb::CreateExportSettings: {
		if (FindExport(scene, created) == nullptr)
			return false; // detached (tombstoned)
		break;
	}
	case EditVerb::CreateTransform:
		// No links to detach: alive == not deleted (checked above).
		break;
	case EditVerb::CreateTerrain: {
		auto git = app.m_terrainGroups.find(created);
		if (git == app.m_terrainGroups.end() || git->second.deleted)
			return false; // deleted (tombstoned) group
		break;
	}
	default:
		return false;
	}
	return ParentResolvesInTour(app, CreateEmitParent(app, h));
}

uint32_t EditorApp::CreateEmitParent(const EditorApp& app, const EditCmd& h) {
	// Live-parent emission (see the header note): the hierarchy IS the
	// current parenting (reparents, undos and replays all land there),
	// so the file carries it directly instead of relying on a Parent
	// verb the child may not be addressable by (editor-created children
	// own no cluster id — their Parent lines could never persist).
	if (!app.m_world)
		return UINT32_MAX;
	const Scene& scene = app.m_world->scene;
	if (h.node == UINT32_MAX || h.node >= scene.hierarchy.size())
		return UINT32_MAX;
	const int p = scene.hierarchy[h.node].parent;
	return p < 0 ? 0 : uint32_t(p); // detached reads as the world root
}

bool EditorApp::IsCreatedChild(const EditorApp& app,
	uint32_t childNode) {
	// Any history create targeting this child (the six create verbs):
	// the child's Parent verbs never persist as Parent lines (see the
	// header note) and skip silently at save.
	for (size_t i = 0; i < app.m_historyCursor
		&& i < app.m_history.size(); ++i) {
		const EditCmd& h = app.m_history[i];
		if (h.node != childNode)
			continue;
		if (h.verb == EditVerb::CreateLight
			|| h.verb == EditVerb::CreateGiVolume
			|| h.verb == EditVerb::CreateCubemap
			|| h.verb == EditVerb::CreateCamera
			|| h.verb == EditVerb::CreateTransform
			|| h.verb == EditVerb::CreateTerrain
			|| h.verb == EditVerb::CreateExportSettings)
			return true;
	}
	return false;
}

bool EditorApp::ParentResolvesInTour(const EditorApp& app, uint32_t parentSlot) {
	if (!app.m_world)
		return false;
	const Scene& scene = app.m_world->scene;
	if (parentSlot == UINT32_MAX || parentSlot >= scene.hierarchy.size())
		return false;
	if (parentSlot == 0)
		return true; // world root: -p 0 (the primary cluster's root)
	if (app.WorldToCluster(parentSlot) >= 0)
		return true; // cluster-backed: numeric -p (the bind builds it)
	// Editor-created parent: it emits only through its OWN create —
	// terrain TILES ride their group's createTerrain (the tile is not
	// a standalone create in history).
	if (app.TerrainGroupOf(parentSlot) != UINT32_MAX
		&& !app.m_terrainGroups.contains(parentSlot)) {
		// A tile of a (possibly tombstoned) group: resolvable exactly
		// when the group's createTerrain emits.
		const uint32_t group = app.TerrainGroupOf(parentSlot);
		for (size_t i = 0; i < app.m_historyCursor
			&& i < app.m_history.size(); ++i) {
			const EditCmd& h = app.m_history[i];
			if (h.verb == EditVerb::CreateTerrain && h.node == group)
				return CreateCmdEmitsInTour(app, h);
		}
		return false;
	}
	// Any other editor-created node: the history create that made it
	// must emit (recursion: the create's own parent must resolve).
	for (size_t i = 0; i < app.m_historyCursor
		&& i < app.m_history.size(); ++i) {
		const EditCmd& h = app.m_history[i];
		if (h.node != parentSlot)
			continue;
		if (h.verb == EditVerb::CreateLight
			|| h.verb == EditVerb::CreateGiVolume
			|| h.verb == EditVerb::CreateCubemap
			|| h.verb == EditVerb::CreateCamera
			|| h.verb == EditVerb::CreateTransform
			|| h.verb == EditVerb::CreateTerrain
			|| h.verb == EditVerb::CreateExportSettings)
			return CreateCmdEmitsInTour(app, h);
	}
	return false; // not cluster-backed, not a create: unaddressable
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
	c.lightShadow = true; // spots + points both default to shadow-casting
	Do(c);
}

void EditorApp::DraftCreateCamera() {
	if (!m_world) {
		Log("Create camera: no scene open.");
		return;
	}
	EditCmd c{};
	c.verb = EditVerb::CreateCamera;
	c.node = UINT32_MAX; // assigned at forward execution
	const uint32_t sel = m_selected;
	c.node2 = (sel != UINT32_MAX && sel < m_world->scene.hierarchy.size())
		? sel : DefaultCreateParent();
	c.afterText = UniqueCameraName();
	// Spawn at the active camera target, aimed back at the active camera
	// eye (so the first look-through frames what you were seeing).
	const EditorCam& cam = ActiveCam();
	c.afterVec = cam.target;
	const glm::vec3 eye = EditorCamEye(cam);
	glm::vec3 d = cam.target - eye;
	if (glm::length(d) < 1e-6f)
		d = glm::vec3(0.0f, 0.0f, -1.0f);
	d = glm::normalize(d);
	// Yaw/pitch for a -Z-forward node (editor TRS convention): yaw about
	// Y from -Z, pitch about X. Matches the orbit-frame math below.
	c.camRot = glm::vec3(glm::degrees(glm::asin(glm::clamp(d.y, -1.0f, 1.0f))),
		glm::degrees(glm::atan(-d.x, -d.z)), 0.0f);
	c.camProjection = (cam.type == EditorCam::Type::Orthographic) ? 1u : 0u;
	c.camFovY = std::clamp(cam.fovYDeg, 1.0f, 179.0f);
	c.camNear = std::max(cam.nearClip, 1e-4f);
	c.camFar = std::max(cam.farClip, c.camNear * 1.1f + 1e-3f);
	c.camOrthoHalfH = std::clamp(cam.orthoHalfH, 1e-2f, 1e5f);
	Do(c);
}

bool EditorApp::IsLocatorNode(const Scene& scene, uint32_t node) {
	// The "locator" definition: a bare transform — no type link of any
	// kind. This is the INVERSE of the contains-chain the outliner uses
	// for tags; it catches editor-created locators, imported glTF empty
	// group nodes, and tour replayed createNode -at transform nodes.
	// Hidden/flagged nodes still count (the flags are display state, not
	// type identity). A terrain group node also matches (its tiles carry
	// the meshes) — see TerrainGroupOf for the distinction when needed.
	if (node >= scene.hierarchy.size())
		return false;
	if (FindMesh(scene, node) != nullptr)
		return false;
	if (FindLight(scene, node) != nullptr)
		return false;
	if (FindCamera(scene, node) != nullptr)
		return false;
	if (FindDecal(scene, node) != nullptr)
		return false;
	if (FindGiVolume(scene, node) != nullptr)
		return false;
	if (FindCubemap(scene, node) != nullptr)
		return false;
	if (FindExport(scene, node) != nullptr)
		return false;
	if (FindEnv(scene, node) != nullptr)
		return false;
	if (FindEnvVolume(scene, node) != nullptr)
		return false;
	return true;
}

void EditorApp::DraftCreateLocator() {
	if (!m_world) {
		Log("Create locator: no scene open.");
		return;
	}
	// A locator is a Create Transform with a meaningful default name —
	// the NAME is the identity game code will look up (findNodeByName).
	// The tour verb is createNode -at transform; the pak scene blob
	// serializes the node's hierarchy row + transform + name with ZERO
	// payload, so it round-trips with no format change.
	EditCmd c{};
	c.verb = EditVerb::CreateTransform;
	c.node = UINT32_MAX; // assigned at forward execution
	const uint32_t sel = m_selected;
	c.node2 = (sel != UINT32_MAX && sel < m_world->scene.hierarchy.size())
		? sel : DefaultCreateParent();
	c.afterText = UniqueLocatorName();
	// Spawn at the active camera target (what you're looking at — the
	// most likely intent for a spawn point / trigger / patrol marker).
	const EditorCam& cam = ActiveCam();
	c.afterVec = cam.target;
	c.vec4 = glm::vec4(0.0f); // no rotation
	c.svec = glm::vec3(1.0f); // unit scale
	Do(c);
}

void EditorApp::ExecCreateLight(EditCmd& c) {
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
	scene.localTransform[size_t(created)] =
		glm::translate(glm::mat4(1.0f), c.afterVec);
	// Name (the nodeNames pool is grow-only, like Rename).
	const uint32_t nameId = uint32_t(scene.nodeNames.size());
	scene.nodeNames.push_back(c.afterText);
	SetNodeNameIndex(scene, uint32_t(created), nameId);
	// Pool definition on the DYNAMIC cluster (World::EnsureDynamicCluster):
	// runtime-created entities live there, never in the scene's cluster —
	// pak unloads leave them alone and the scene cluster stays pure cooked
	// content. FindNodeLight resolves world lights through lightLocations
	// -> cluster + handle, so nothing downstream changes.
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
	if (c.lightShadow && def.type != ::SceneLight::Type::Directional)
		def.flags |= ::SceneLight::kSceneLightFlag_CastShadow;
	def.name = c.afterText;
	smvk::Cluster* cl = &m_world->EnsureDynamicCluster(m_context.get());
	auto handle = cl->lights.create(std::move(def));
	const uint32_t worldLightIdx = uint32_t(m_world->lightLocations.size());
	m_world->lightLocations.push_back({ cl, handle });
	SetLight(scene, uint32_t(created), worldLightIdx);
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

// Shared create-undo retirement (Epoch 6b — see the header contract):
// the childless case physically frees the slot; every OTHER node id
// stays put and a redo re-creates at the same slot id (the freelist
// pops LIFO). World draw caches + selection scrub for the retired
// node — the scene arrays no longer shrink past it the way the old
// pop-back did by accident.
void EditorApp::RetireCreatedNode(uint32_t n) {
	if (!m_world || n == UINT32_MAX || n >= m_world->scene.hierarchy.size())
		return;
	Scene& scene = m_world->scene;
	if (!IsNodeAlive(scene, n))
		return; // already retired (double undo): no-op
	const int p = scene.hierarchy[n].parent;
	UnlinkNodeFromParent(scene, int(n));
	RetireNodeSlot(scene, n);
	// World draw caches (the ExecDelete discipline): the retired slot
	// keeps its flags[]/drawItems entries otherwise — flat iteration
	// must never see it again.
	if (n < m_world->flags.size())
		m_world->flags[n] = smvk::IteratorFlags(smvk::IteratorFlagBit_None);
	for (size_t i = m_world->drawItems.size(); i-- > 0;)
		if (m_world->drawItems[i].node == n) {
			m_world->drawItems.erase(m_world->drawItems.begin()
				+ ptrdiff_t(i));
			m_world->itemFlags.erase(m_world->itemFlags.begin()
				+ ptrdiff_t(i));
		}
	m_world->flagLists.clear();
	recalculateGlobalTransforms(scene);
	// Selection scrub: a retired id must not stay selected.
	for (size_t i = m_selection.size(); i-- > 0;)
		if (m_selection[i] == n)
			m_selection.erase(m_selection.begin() + ptrdiff_t(i));
	if (m_selected == n)
		m_selected = (p >= 0 && IsNodeAlive(scene, uint32_t(p)))
			? uint32_t(p)
			: (m_selection.empty() ? UINT32_MAX : m_selection.back());
}

void EditorApp::ExecUncreateLight(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	const bool childless = scene.hierarchy[n].firstChild < 0;
	const LightRow* lit = FindLight(scene, n);
	const bool lightTail = lit != nullptr
		&& lit->light + 1 == m_world->lightLocations.size();
	if (childless) {
		// Epoch 6b: physical RETIREMENT anywhere in the graph (the old
		// tail-pop only worked on the childless tail; mid-graph uncreates
		// left hidden zombies). The pool definition + location row pop
		// when the def is the pool tail (unchanged).
		if (lit != nullptr) {
			if (lightTail) {
				const auto& loc = m_world->lightLocations[lit->light];
				if (loc.cluster)
					loc.cluster->lights.destroy(loc.handle);
				m_world->lightLocations.pop_back();
			}
			RemoveLight(scene, n);
		}
		RetireCreatedNode(n);
		Log("Undid light creation (#" + std::to_string(n)
			+ "; slot retired, ids stable).");
	} else {
		// Children live under the node: the slot must stay alive for
		// them — tombstone (hide + detach) as before.
		MarkHidden(scene, int(n));
		RemoveLight(scene, n);
		Log("Undid light creation (#" + std::to_string(n)
		 + "): hidden, ids kept stable (pool entry retained).");
	}
}

// ---------------------------------------------------------------------------
// Created cameras.
//
// A created camera is a WORLD node appended to the live scene plus a
// SceneCamera lens definition in the primary cluster's cameras pool,
// linked through world cameraLocations — so gizmos, the inspector, the
// outliner tags and the look-through path all pick it up with no special
// cases. Undo removes the node when it is still the childless tail (ids
// stay stable); otherwise the camera is tombstoned (hidden + detached)
// for the same reason.
// Tour persistence: CreateCamera carries the creation (CmdToTour emits
// the LIVE pose + lens, so post-creation edits ride along); SetCamAttr
// addresses later creates via kCreateCameraRefBase (creation order).
// ---------------------------------------------------------------------------
void EditorApp::ExecCreateCamera(EditCmd& c) {
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
	// TRS = translate + rotation-deg (editor convention; scale stays 1).
	scene.localTransform[size_t(created)] = LocalMatrixFromTRS(
		c.afterVec, c.camRot, glm::vec3(1.0f));
	// Name (the nodeNames pool is grow-only, like Rename).
	const uint32_t nameId = uint32_t(scene.nodeNames.size());
	scene.nodeNames.push_back(c.afterText);
	SetNodeNameIndex(scene, uint32_t(created), nameId);
	// Lens definition on the DYNAMIC cluster (World::EnsureDynamicCluster:
	// runtime-created entities live there — see ExecCreateLight).
	// FindNodeCamera resolves world cameras through cameraLocations ->
	// cluster + handle.
	::SceneCamera def{};
	def.projection = (c.camProjection != 0)
		? ::SceneCamera::Projection::Orthographic
		: ::SceneCamera::Projection::Perspective;
	def.fovYDeg = std::clamp(c.camFovY, 1.0f, 179.0f);
	def.nearClip = std::max(c.camNear, 1e-4f);
	def.farClip = std::max(c.camFar, def.nearClip * 1.1f + 1e-3f);
	def.orthoHalfH = std::clamp(c.camOrthoHalfH, 1e-2f, 1e5f);
	def.name = c.afterText;
	smvk::Cluster* cl = &m_world->EnsureDynamicCluster(m_context.get());
	auto handle = cl->cameras.create(std::move(def));
	const uint32_t worldCamIdx = uint32_t(m_world->cameraLocations.size());
	m_world->cameraLocations.push_back({ cl, handle });
	SetCamera(scene, uint32_t(created), worldCamIdx);
	if (m_replayingTour)
		m_tourCreatedCameraNodes.push_back(uint32_t(created));
	markAsChanged(scene, created);
	recalculateGlobalTransforms(scene);
	m_selected = uint32_t(created);
	char buf[160];
	std::snprintf(buf, sizeof(buf), "Created %s camera '%s' (#%u)",
		c.camProjection != 0 ? "ortho" : "perspective",
		c.afterText.c_str(), uint32_t(created));
	Log(buf);
}

void EditorApp::ExecUncreateCamera(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	const bool childless = scene.hierarchy[n].firstChild < 0;
	const CameraRow* cit = FindCamera(scene, n);
	const bool camTail = cit != nullptr
		&& cit->camera + 1 == m_world->cameraLocations.size();
	if (childless) {
		// Epoch 6b retirement (see ExecUncreateLight): the slot frees
		// anywhere in the graph; the pool def + location row pop when
		// the def is the pool tail (unchanged).
		if (cit != nullptr) {
			if (camTail) {
				const auto& loc = m_world->cameraLocations[cit->camera];
				if (loc.cluster)
					loc.cluster->cameras.destroy(loc.handle);
				m_world->cameraLocations.pop_back();
			}
			RemoveCamera(scene, n);
		}
		RetireCreatedNode(n);
		Log("Undid camera creation (#" + std::to_string(n)
			+ "; slot retired, ids stable).");
	} else {
		// Children live under the node: tombstone (hide + detach).
		MarkHidden(scene, int(n));
		RemoveCamera(scene, n);
		Log("Undid camera creation (#" + std::to_string(n) +
		 "): hidden, ids kept stable (pool entry retained).");
	}
}

// ---------------------------------------------------------------------------
// Created GI volumes (GI volume nodes).
//
// A created GI volume is a WORLD transform node (translate = box center,
// scale = full extents, axis-aligned — rotation ignored by design) plus
// a node-local SceneGiVolume definition in the scene's GI-volume
// table — so gizmos,
// the inspector, the outliner and the tour all pick it up with no
// special cases. Undo removes the node when it is still the childless
// tail (ids stay stable); otherwise the node is tombstoned (hidden +
// def detached) for the same reason.
// Tour persistence: CreateGiVolume carries the creation (CmdToTour emits
// the LIVE params, so post-creation edits ride along); SetGiVolumeAttr
// addresses later creates via kCreateGiVolumeRefBase (creation order).
// ---------------------------------------------------------------------------
void EditorApp::DraftCreateGiVolume() {
	if (!m_world) {
		Log("Create GI volume: no scene open.");
		return;
	}
	EditCmd c{};
	c.verb = EditVerb::CreateGiVolume;
	c.node = UINT32_MAX; // assigned at forward execution
	const uint32_t sel = m_selected;
	c.node2 = (sel != UINT32_MAX && sel < m_world->scene.hierarchy.size())
		? sel : DefaultCreateParent();
	c.afterText = UniqueGiVolumeName();
	c.afterVec = ActiveCam().target;
	c.svec = glm::vec3(8.0f); // 8 m authoring box
	c.giVolumePriority = 0;
	c.giVolumeSpacing = 0.5f;
	c.giVolumeInsideFade = 0.1f;
	c.giVolumeOutsideFade = 0.1f;
	Do(c);
}

void EditorApp::ExecCreateGiVolume(EditCmd& c) {
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
	SetNodeNameIndex(scene, uint32_t(created), nameId);
	::SceneGiVolume def{};
	def.priority = c.giVolumePriority;
	def.spacing = std::max(c.giVolumeSpacing, 1e-3f);
	def.insideFade = std::max(c.giVolumeInsideFade, 0.0f);
	def.outsideFade = std::max(c.giVolumeOutsideFade, 0.0f);
	SetGiVolume(scene, uint32_t(created), def);
	if (m_replayingTour)
		m_tourCreatedGiVolumeNodes.push_back(uint32_t(created));
	markAsChanged(scene, created);
	recalculateGlobalTransforms(scene);
	m_selected = uint32_t(created);
	char buf[160];
	std::snprintf(buf, sizeof(buf), "Created GI volume '%s' (#%u)",
		c.afterText.c_str(), uint32_t(created));
	Log(buf);
}

void EditorApp::ExecUncreateGiVolume(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	const bool childless = scene.hierarchy[n].firstChild < 0;
	if (childless) {
		// Epoch 6b retirement (see ExecUncreateLight): the slot frees
		// anywhere in the graph, every other id stays put.
		RemoveGiVolume(scene, n);
		RetireCreatedNode(n);
		Log("Undid GI volume creation (#" + std::to_string(n)
			+ "; slot retired, ids stable).");
	} else {
		// Children live under the node: tombstone (hide + detach).
		MarkHidden(scene, int(n));
		RemoveGiVolume(scene, n);
		Log("Undid GI volume creation (#" + std::to_string(n) +
			"): hidden, ids kept stable (def retained in the command).");
	}
}

// ---------------------------------------------------------------------------
// The exportSettings node (the scene's WorldSettings-style export config).
//
// A created node is a WORLD transform node (identity; a config node, no
// shape) plus a node-local SceneExportSettings payload in
// scene.exportForNode — the inspector, the outliner, the tour (createNode
// -at exportSettings + setExportAttr) and the cooks all pick it up with
// no special cases. The AUTO-SEED singleton: EnsureExportSettingsNode
// plants one on every scene open (undoable like any create; the next
// open re-seeds). Undo removes the node when it is still the childless
// tail (ids stay stable); otherwise the node is tombstoned (hidden +
// payload detached).
// ---------------------------------------------------------------------------
uint32_t EditorApp::FindExportSettingsNode() const {
	if (!m_world)
		return UINT32_MAX;
	const Scene& scene = m_world->scene;
	// The FIRST in node id order = THE one (the singleton rule; the
	// auto-seed keeps it at exactly one — duplicates only via hand
	// edits or referenced scenes, first wins like every consumer).
	uint32_t best = UINT32_MAX;
	for (const ExportRow& _row : scene.tables.exports) {
		const NodeId n = _row.node; const auto& def = _row.settings;
		(void)def;
		if (n < scene.hierarchy.size() && n < best)
			best = n;
	}
	return best;
}

const SceneExportSettings& EditorApp::ExportCfg() const {
	// The live export settings: the node's payload, or the defaults when
	// the scene carries none (transient — the auto-seed fires on every
	// scene open; "none" means a mid-session undo removed the node).
	static const SceneExportSettings kDefaults{};
	if (!m_world)
		return kDefaults;
	const Scene& scene = m_world->scene;
	const uint32_t n = FindExportSettingsNode();
	if (n == UINT32_MAX)
		return kDefaults;
	auto it = FindExport(scene, n);
	return it ? it->settings : kDefaults;
}

void EditorApp::EnsureExportSettingsNode() {
	// WorldSettings scaffold: every open scene carries exactly one
	// exportSettings node. Joined as a plain (undoable) create so the
	// save emits it like any created node; the singleton rule guards
	// duplicates.
	if (!m_world)
		return;
	if (FindExportSettingsNode() != UINT32_MAX)
		return;
	DraftCreateExportSettings();
}

void EditorApp::DraftCreateExportSettings() {
	if (!m_world) {
		Log("Create exportSettings: no scene open.");
		return;
	}
	EditCmd c{};
	c.verb = EditVerb::CreateExportSettings;
	c.node = UINT32_MAX; // assigned at forward execution
	c.node2 = DefaultCreateParent();
	c.afterText = "ExportSettings";
	Do(c);
}

void EditorApp::ExecCreateExportSettings(EditCmd& c) {
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
	scene.localTransform[size_t(created)] = glm::mat4(1.0f);
	const uint32_t nameId = uint32_t(scene.nodeNames.size());
	scene.nodeNames.push_back(
		c.afterText.empty() ? "ExportSettings" : c.afterText);
	SetNodeNameIndex(scene, uint32_t(created), nameId);
	SetExport(scene, uint32_t(created), SceneExportSettings{});
	if (m_replayingTour)
		m_tourCreatedExportNodes.push_back(uint32_t(created));
	markAsChanged(scene, created);
	recalculateGlobalTransforms(scene);
	char buf[160];
	std::snprintf(buf, sizeof(buf), "Created exportSettings '%s' (#%u)",
		c.afterText.c_str(), uint32_t(created));
	Log(buf);
}

void EditorApp::ExecUncreateExportSettings(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	const bool childless = scene.hierarchy[n].firstChild < 0;
	if (childless) {
		// Epoch 6b retirement (the ExecUncreateGiVolume pattern): the
		// slot frees anywhere in the graph, every other id stays put.
		RemoveExport(scene, n);
		RetireCreatedNode(n);
		Log("Undid exportSettings creation (#" + std::to_string(n)
			+ "; slot retired, ids stable).");
	} else {
		// Children live under the node: tombstone (hide + detach).
		MarkHidden(scene, int(n));
		RemoveExport(scene, n);
		Log("Undid exportSettings creation (#" + std::to_string(n) +
			"): hidden, ids kept stable (payload retained in the command).");
	}
}

bool EditorApp::ReadNodeExportAttr(uint32_t node, const std::string& attr,
	double& num, bool& flag, std::string& text) const {
	num = 0.0;
	flag = false;
	text.clear();
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return false;
	const Scene& scene = m_world->scene;
	auto it = FindExport(scene, node);
	if (!it)
		return false;
	const SceneExportSettings& es = it->settings;
	if (attr == "meshopt") flag = es.meshopt;
	else if (attr == "lods") flag = es.lods;
	else if (attr == "lodRatios") text = es.lodRatios;
	else if (attr == "lodError") num = double(es.lodError);
	else if (attr == "lightmap") flag = es.lightmap;
	else if (attr == "lmMetersPerTexel") num = double(es.lmMetersPerTexel);
	else if (attr == "lmLobes") num = double(es.lmLobes);
	else if (attr == "lmAtlasWidth") num = double(es.lmAtlasWidth);
	else if (attr == "lmRays") num = double(es.lmRays);
	else if (attr == "lmProxyRes") num = double(es.lmProxyRes);
	else if (attr == "lmExcludeSmall") num = double(es.lmExcludeSmall);
	else if (attr == "lmExcludeKw") text = es.lmExcludeKw;
	else if (attr == "cubemap") flag = es.cubemap;
	else if (attr == "cubemapSize") num = double(es.cubemapSize);
	else if (attr == "cubemapRays") num = double(es.cubemapRays);
	else if (attr == "cubemapOctree") flag = es.cubemapOctree;
	else if (attr == "cubemapOctreeMax") num = double(es.cubemapOctreeMax);
	else if (attr == "cubemapOctreeMin") num = double(es.cubemapOctreeMin);
	else if (attr == "cubemapOctreeDepth") num = double(es.cubemapOctreeDepth);
	else if (attr == "cubemapExtraRadius") num = double(es.cubemapExtraRadius);
	else if (attr == "meshlets") flag = es.meshlets;
	else if (attr == "meshletMaxVerts") num = double(es.meshletMaxVerts);
	else if (attr == "meshletMaxTris") num = double(es.meshletMaxTris);
	else if (attr == "meshletConeWeight") num = double(es.meshletConeWeight);
	else if (attr == "giVolumes") flag = es.giVolumes;
	// ("giWorld" reads as unknown here — the field is retired with the
	// .sggi octree; the tour PARSE still tolerates the token, so old
	// files load, but the editor can no longer author it.)
	else if (attr == "autoKtx") flag = es.autoKtx;
	else if (attr == "ignoreVfs") flag = es.ignoreVfs;
	else return false;
	return true;
}

bool EditorApp::WriteNodeExportAttr(uint32_t node, const std::string& attr,
	double num, bool flag, const std::string& text) {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return false;
	Scene& scene = m_world->scene;
	auto it = FindExport(scene, node);
	if (!it)
		return false;
	// The ONE mapping runs through the shared ApplyExportAttr (the same
	// code the tour replay + Cook.exe run — no drift by construction).
	smvk::tour::Command tc{};
	tc.verb = smvk::tour::Verb::SetExportAttr;
	tc.text = attr;
	tc.number = num;
	tc.flag = flag;
	tc.name = text;
	(void)smvk::tour::ApplyExportAttr(it->settings, tc, nullptr);
	return true;
}

void EditorApp::PushExportAttr(uint32_t node, const std::string& attr,
	double num, bool flag, const std::string& text) {
	EditCmd c{};
	c.verb = EditVerb::SetExportAttr;
	c.node = node;
	c.exportAttr = attr;
	c.number = num;
	c.afterFlag = flag;
	c.afterText = text;
	ReadNodeExportAttr(node, attr, c.beforeNum, c.beforeFlag, c.beforeText);
	Do(c);
}

int EditorApp::CreatedExportIndex(uint32_t worldNode) const {
	// Same contract as CreatedGiVolumeIndex, for the exportSettings family:
	// alive + emitting creates only.
	int count = 0;
	for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
		const EditCmd& h = m_history[i];
		if (h.verb != EditVerb::CreateExportSettings)
			continue;
		if (!m_world || !(FindExport(m_world->scene, h.node) != nullptr))
			continue; // deleted (tombstoned): not in the file's ref space
		if (!CreateCmdEmitsInTour(*this, h))
			continue; // emission dropped (parent unresolvable)
		if (h.node == worldNode)
			return count;
		++count;
	}
	return -1;
}

// ---------------------------------------------------------------------------
// Created cubemap captures (pinned specular probe nodes).
//
// A created capture is a WORLD transform node (translate = the pinned
// probe position) plus a node-local SceneCubemap definition in
// scene.cubemapForNode — gizmos, the inspector, the outliner and the
// cook's --cubemap harvest all pick it up with no special cases. Undo
// removes the node when it is still the childless tail (ids stay
// stable); otherwise the node is tombstoned (hidden + def detached).
// Tour persistence: CreateCubemap carries the creation (CmdToTour
// emits the LIVE params); SetCubemapAttr addresses later creates via
// kCreateCubemapRefBase (creation order).
// ---------------------------------------------------------------------------
void EditorApp::DraftCreateCubemap() {
	if (!m_world) {
		Log("Create cubemap capture: no scene open.");
		return;
	}
	EditCmd c{};
	c.verb = EditVerb::CreateCubemap;
	c.node = UINT32_MAX; // assigned at forward execution
	const uint32_t sel = m_selected;
	c.node2 = (sel != UINT32_MAX && sel < m_world->scene.hierarchy.size())
		? sel : DefaultCreateParent();
	c.afterText = UniqueCubemapName();
	// Use active cam target as default position; user can override via gizmo
	c.afterVec = ActiveCam().target;
	c.cubemapRadius = 1.5f;
	Do(c);
}

void EditorApp::ExecCreateCubemap(EditCmd& c) {
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
	// TRS = translate only (the pinned probe position; scale/rotation
	// carry no meaning for a capture).
	scene.localTransform[size_t(created)] =
		glm::translate(glm::mat4(1.0f), c.afterVec);
	const uint32_t nameId = uint32_t(scene.nodeNames.size());
	scene.nodeNames.push_back(c.afterText);
	SetNodeNameIndex(scene, uint32_t(created), nameId);
	::SceneCubemap def{};
	def.radius = std::max(c.cubemapRadius, 1e-2f);
	SetCubemap(scene, uint32_t(created), def);
	if (m_replayingTour)
		m_tourCreatedCubemapNodes.push_back(uint32_t(created));
	markAsChanged(scene, created);
	recalculateGlobalTransforms(scene);
	m_selected = uint32_t(created);
	char buf[160];
	std::snprintf(buf, sizeof(buf), "Created cubemap capture '%s' (#%u)",
		c.afterText.c_str(), uint32_t(created));
	Log(buf);
}

void EditorApp::ExecUncreateCubemap(EditCmd& c) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	const uint32_t n = c.node;
	if (n == UINT32_MAX || n >= scene.hierarchy.size())
		return;
	const bool childless = scene.hierarchy[n].firstChild < 0;
	if (childless) {
		// Epoch 6b retirement (see ExecUncreateLight): the slot frees
		// anywhere in the graph, every other id stays put.
		RemoveCubemap(scene, n);
		RetireCreatedNode(n);
		Log("Undid cubemap creation (#" + std::to_string(n)
			+ "; slot retired, ids stable).");
	} else {
		// Children live under the node: tombstone (hide + detach).
		MarkHidden(scene, int(n));
		RemoveCubemap(scene, n);
		Log("Undid cubemap creation (#" + std::to_string(n) +
			"): hidden, ids kept stable (def retained in the command).");
	}
}

// ---------------------------------------------------------------------------
// Delete (Del key / Edit menu, Maya delete): remove a node from the LIVE
// scene. Links drop — the mesh/material tables (draw state), lightForNode
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
			if (const MeshRow* m = FindMesh(scene, id)) {
				hasMesh = 1;
				mesh = m->mesh;
			}
			if (const MaterialRow* a = FindMaterial(scene, id)) {
				hasMat = 1;
				mat = a->material;
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
		auto lit = FindLight(scene, n);
		c.number = (lit)
			? double(lit->light) : -1.0;
		// GI-volume def (created or replayed): node-local floats ride
		// beforeVec4 (priority, spacing, inside, outside) + presence
		// rides beforeFlag — the light-index precedent for the exact
		// undo below.
		auto cvit = FindGiVolume(scene, n);
		c.beforeFlag = (cvit);
		if (c.beforeFlag)
			c.beforeVec4 = glm::vec4(float(cvit->volume.priority),
				cvit->volume.spacing, cvit->volume.insideFade,
				cvit->volume.outsideFade);
		// Cubemap capture def (created or replayed): presence rides
		// beforeCubemap, the radius rides beforeNum (the light index
		// in c.number above is exclusive — a capture node has no
		// light; a light node has no capture).
		auto cmit = FindCubemap(scene, n);
		c.beforeCubemap = (cmit);
		if (c.beforeCubemap)
			c.beforeNum = double(cmit->cubemap.radius);
		// Camera def (created or replayed): world camera index rides
		// beforeCamIdx + presence rides beforeCamera; the pool entry
		// itself stays alive like lights (links only).
		auto camit = FindCamera(scene, n);
		c.beforeCamera = (camit);
		c.beforeCamIdx = c.beforeCamera ? double(camit->camera) : -1.0;
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
		RemoveMesh(scene, id);
		RemoveMaterial(scene, id);
		RemoveGiVolume(scene, id);
		RemoveCubemap(scene, id);
		RemoveCamera(scene, id);
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
	RemoveLight(scene, n);
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
	// Restore the captured links (order-free): mesh/material tables and
	// deleted-set membership per node, the light link, then the draw
	// entries at their ORIGINAL positions in ascending order (every
	// earlier reinsert already restored its shift, so each index lands
	// exactly where it was).
	for (size_t k = 0; k + 4 < c.delNodeMaps.size(); k += 5) {
		const uint32_t id = c.delNodeMaps[k];
		// The flag branches only wrote back on presence; the else-side
		// Remove* is a safe no-op (deletion already dropped any row)
		// and keeps "absent before the delete" ending with NO row.
		if (c.delNodeMaps[k + 1])
			SetMesh(scene, id, c.delNodeMaps[k + 2]);
		else
			RemoveMesh(scene, id);
		if (c.delNodeMaps[k + 3])
			SetMaterial(scene, id, c.delNodeMaps[k + 4]);
		else
			RemoveMaterial(scene, id);
		m_deletedNodes.erase(id);
	}
	if (c.number >= 0.0 && c.number < double(m_world->lightLocations.size()))
		SetLight(scene, n, uint32_t(c.number));
	if (c.beforeFlag)
		SetGiVolume(scene, n, ::SceneGiVolume{ uint32_t(c.beforeVec4.x),
			c.beforeVec4.y, c.beforeVec4.z, c.beforeVec4.w });
	if (c.beforeCubemap)
		SetCubemap(scene, n, ::SceneCubemap{ float(c.beforeNum) });
	if (c.beforeCamera && c.beforeCamIdx >= 0.0
		&& c.beforeCamIdx < double(m_world->cameraLocations.size()))
		SetCamera(scene, n, uint32_t(c.beforeCamIdx));
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
	// Reference placeholders in this Delete batch: Delete removes the
	// whole reference (RemoveSceneReferencesAt, one rebind), not the
	// node. Pre-scanned so subtree members below a doomed placeholder
	// can be skipped inside the loop.
	std::vector<size_t> refIdxs;
	std::vector<uint32_t> refPhNodes;
	for (uint32_t t : targets) {
		if (t == UINT32_MAX || !m_world
			|| t >= m_world->scene.hierarchy.size())
			continue;
		if (!IsRefPlaceholder(t))
			continue;
		auto it = std::find(m_refPlaceholders.begin(),
			m_refPlaceholders.end(), t);
		if (it != m_refPlaceholders.end()
			&& std::find(refPhNodes.begin(), refPhNodes.end(), t)
				== refPhNodes.end())
			refPhNodes.push_back(t);
	}
	auto underRefRemoval = [&](uint32_t n) {
		if (refPhNodes.empty() || !m_world)
			return false;
		for (uint32_t a = n;
			a < m_world->scene.hierarchy.size();) {
			for (uint32_t ph : refPhNodes)
				if (a == ph)
					return true;
			const int p = m_world->scene.hierarchy[a].parent;
			if (p < 0)
				return false;
			a = uint32_t(p);
		}
		return false;
	};
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
			// The placeholder IS the reference binding anchor: Delete
			// removes the whole reference (definition + reload), not
			// just the node. Collected below; bulk-removed after the
			// ordinary deletes so one rebind covers everything.
			auto it = std::find(m_refPlaceholders.begin(),
				m_refPlaceholders.end(), n);
			if (it == m_refPlaceholders.end()) {
				Log("Delete: '" + getNodeName(m_world->scene, int(n)) +
					"' is a reference placeholder — remove the reference "
					"itself (References panel).");
				continue;
			}
			refIdxs.push_back(
				size_t(std::distance(m_refPlaceholders.begin(), it)));
			if (std::find(refPhNodes.begin(), refPhNodes.end(), n)
				== refPhNodes.end())
				refPhNodes.push_back(n);
			continue;
		}
		// A node inside a reference subtree that is itself being
		// removed vanishes with the reference — a per-node delete
		// on top would only leave an orphan command behind.
		if (underRefRemoval(n)) {
			Log("Delete: '" + getNodeName(m_world->scene, int(n)) +
				"' goes away with its reference (skipped).");
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
	// Reference removals last: ordinary deletes above are already in
	// the history, so the removal's temp-snapshot reload carries them.
	// One rebind covers the whole batch (per-index removal would
	// reload N times).
	if (!refIdxs.empty()) {
		std::sort(refIdxs.begin(), refIdxs.end());
		refIdxs.erase(std::unique(refIdxs.begin(), refIdxs.end()),
			refIdxs.end());
		RemoveSceneReferencesAt(std::move(refIdxs));
	}
}

// ---------------------------------------------------------------------------
// Duplicate (Edit > Duplicate / Ctrl+D): a fresh Create per selected
// duplicable node, seeded from the LIVE state (post-creation edits ride
// along exactly like the Create-menu drafts read the world). Editor-
// created families only: cluster-backed mesh nodes and terrain groups/
// tiles own no create verb (imported content would need an instancing
// system; a duplicated terrain would silently lose its paint stamps).
// The new nodes become the selection, like the Create-menu drafts.
// ---------------------------------------------------------------------------
void EditorApp::DuplicateSelection() {
	if (!m_world) {
		Log("Duplicate: no scene open.");
		return;
	}
	if (m_worldSelected) {
		Log("Duplicate: 'World' holds global settings (not a scene node).");
		return;
	}
	std::vector<uint32_t> targets(m_selection);
	if (targets.empty() && m_selected != UINT32_MAX)
		targets.push_back(m_selected);
	if (targets.empty()) {
		Log("Duplicate: nothing selected.");
		return;
	}
	std::vector<uint32_t> created; // new ids, in target order
	int skippedMesh = 0, skippedTerrain = 0, skippedRef = 0;
	for (uint32_t t : targets) {
		if (t == UINT32_MAX || t >= m_world->scene.hierarchy.size()
			|| m_deletedNodes.contains(t))
			continue;
		if (IsRefPlaceholder(t)) {
			// The placeholder IS the reference binding anchor —
			// duplicating the reference is the References panel's job.
			++skippedRef;
			continue;
		}
		// Terrain: groups/tiles ride one CreateTerrain (immutable params
		// + TerrainPaint stamps). A duplicate would silently drop the
		// painted state — skip with a log line instead.
		if (TerrainGroupOf(t) != UINT32_MAX) {
			++skippedTerrain;
			continue;
		}
		// Cluster-backed mesh nodes own no create verb (imported
		// geometry — a duplicate needs an instancing system).
		if (FindMesh(m_world->scene, t) != nullptr) {
			++skippedMesh;
			continue;
		}
		// Same parent, live transform, unique copy name.
		const int parent = m_world->scene.hierarchy[t].parent;
		const std::string name = UniqueCopyName(
			getNodeName(m_world->scene, int(t)));
		glm::vec3 tr, rot, sc;
		ReadLocalTRS(t, tr, rot, sc);
		EditCmd c{};
		if (const ::SceneLight* L = FindNodeLight(t, nullptr)) {
			c.verb = EditVerb::CreateLight;
			c.node2 = parent < 0 ? UINT32_MAX : uint32_t(parent);
			c.afterText = name;
			c.afterVec = tr;
			c.lightType = (L->type == ::SceneLight::Type::Spot) ? 2u : 1u;
			c.lightColor = L->color;
			c.lightIntensity = L->intensity;
			c.lightRange = L->range;
			c.lightInnerDeg = glm::degrees(L->innerConeAngle);
			c.lightOuterDeg = glm::degrees(L->outerConeAngle);
			c.lightStatic =
				(L->flags & ::SceneLight::kSceneLightFlag_Static) != 0;
			c.lightShadow =
				(L->flags & ::SceneLight::kSceneLightFlag_CastShadow) != 0;
		}
		else if (const ::SceneCamera* C = FindNodeCamera(t)) {
			c.verb = EditVerb::CreateCamera;
			c.node2 = parent < 0 ? UINT32_MAX : uint32_t(parent);
			c.afterText = name;
			c.afterVec = tr;
			c.camRot = rot;
			c.camProjection =
				(C->projection == ::SceneCamera::Projection::Orthographic)
				? 1u : 0u;
			c.camFovY = C->fovYDeg;
			c.camNear = C->nearClip;
			c.camFar = C->farClip;
			c.camOrthoHalfH = C->orthoHalfH;
		}
		else if (const CubemapRow* cmit = FindCubemap(m_world->scene, t)) {
			c.verb = EditVerb::CreateCubemap;
			c.node2 = parent < 0 ? UINT32_MAX : uint32_t(parent);
			c.afterText = name;
			c.afterVec = tr;
			c.cubemapRadius = cmit->cubemap.radius;
		}
		else if (const GiVolumeRow* cvit = FindGiVolume(m_world->scene, t)) {
			c.verb = EditVerb::CreateGiVolume;
			c.node2 = parent < 0 ? UINT32_MAX : uint32_t(parent);
			c.afterText = name;
			c.afterVec = tr;
			c.svec = sc;
			c.giVolumePriority = cvit->volume.priority;
			c.giVolumeSpacing = cvit->volume.spacing;
			c.giVolumeInsideFade = cvit->volume.insideFade;
			c.giVolumeOutsideFade = cvit->volume.outsideFade;
		}
		else {
			// Plain transform: created transforms AND imported empty
			// group nodes (no links) — both are just a DAG transform.
			c.verb = EditVerb::CreateTransform;
			c.node2 = parent < 0 ? UINT32_MAX : uint32_t(parent);
			c.afterText = name;
			c.afterVec = tr;
			c.vec4 = glm::vec4(rot, 0.0f);
			c.svec = sc;
		}
		Do(c);
		if (c.node != UINT32_MAX)
			created.push_back(c.node);
	}
	if (created.empty()) {
		Log("Duplicate: nothing duplicable in the selection"
			+ std::string(skippedMesh > 0 ? " (imported mesh nodes have"
				" no create verb)" : "")
			+ std::string(skippedTerrain > 0 ? " (terrain duplicates"
				" would lose painted state)" : "")
			+ std::string(skippedRef > 0 ? " (reference placeholders are"
				" not nodes)" : "") + ".");
		return;
	}
	// The creates each auto-selected themselves; make the BATCH the
	// selection (mirrors SetSelection's invariants: world page off,
	// non-DAG/asset selections cleared, primary = the newest).
	m_worldSelected = false;
	ClearNonDagSelection();
	m_assetsSel.clear();
	m_selection = created;
	m_selected = created.back();
	if (skippedMesh + skippedTerrain + skippedRef > 0)
		Log("Duplicate: " + std::to_string(created.size()) + " node"
			+ (created.size() > 1 ? "s" : "") + " duplicated (skipped: "
			+ std::to_string(skippedMesh) + " imported mesh"
			+ (skippedMesh == 1 ? "" : "es") + ", "
			+ std::to_string(skippedTerrain) + " terrain, "
			+ std::to_string(skippedRef) + " reference placeholder"
			+ (skippedRef == 1 ? "" : "s") + ").");
	else
		Log("Duplicate: " + std::to_string(created.size()) + " node"
			+ (created.size() > 1 ? "s" : "") + " duplicated.");
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
	SetNodeNameIndex(scene, uint32_t(created), nameId);
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
	// A transform node carries no links. Epoch 6b: the slot RETIRES
	// anywhere in the graph (the old physical path only worked on the
	// childless tail); a parented-into node keeps its slot for the
	// children under it (hide — the created-light tombstone precedent).
	const bool childless = scene.hierarchy[n].firstChild < 0;
	if (childless) {
		RetireCreatedNode(n);
		Log("Undid transform creation (#" + std::to_string(n)
			+ "; slot retired, ids stable).");
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
	} else if (attr == "cookie") {
		// Spot gobo ref (a .texture path or image URI; empty = none).
		text = L->cookieUri;
	} else if (attr == "function") {
		num = double(L->functionType);
	} else if (attr == "functionSpeed") {
		num = double(L->functionSpeed);
	} else if (attr == "functionScale") {
		num = double(L->functionScale);
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
	auto lit = FindLight(scene, node);
	if (!lit
		|| lit->light >= m_world->lightLocations.size())
		return false;
	const auto& loc = m_world->lightLocations[lit->light];
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
		SetNodeNameIndex(scene, node, id);
	} else if (attr == "cookie") {
		// Spot gobo ref: the string persists verbatim (the light build
		// resolves it to the bindless index; empty = none). Unresolved
		// refs warn + drop there — never fatal (none/black on error).
		L->cookieUri = text;
	} else if (attr == "function") {
		L->functionType = uint32_t(
			std::clamp(int(num), 0, 3)); // 0 none,1 flicker,2 pulse,3 bands
	} else if (attr == "functionSpeed") {
		L->functionSpeed = std::max(float(num), 0.01f);
	} else if (attr == "functionScale") {
		L->functionScale = std::clamp(float(num), 0.0f, 1.0f);
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

bool EditorApp::ReadNodeGiVolumeAttr(uint32_t node, const std::string& attr,
	glm::vec4& v4, double& num, std::string& text) const {
	v4 = glm::vec4(0.0f);
	num = 0.0;
	text.clear();
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return false;
	const Scene& scene = m_world->scene;
	auto it = FindGiVolume(scene, node);
	if (!it)
		return false;
	const ::SceneGiVolume& def = it->volume;
	if (attr == "priority") {
		num = double(def.priority);
	} else if (attr == "spacing") {
		num = double(def.spacing);
	} else if (attr == "spacinglow") {
		num = double(def.spacingLow);
	} else if (attr == "spacinghigh") {
		num = double(def.spacingHigh);
	} else if (attr == "optlayout") {
		// The optimizer's seed: number = spacing, vec4.xyz = phase.
		num = double(def.optSpacing);
		v4 = glm::vec4(def.optPhase[0], def.optPhase[1], def.optPhase[2],
			0.0f);
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

bool EditorApp::WriteNodeGiVolumeAttr(uint32_t node, const std::string& attr,
	const glm::vec4& v4, double num, const std::string& text) {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return false;
	Scene& scene = m_world->scene;
	auto it = FindGiVolume(scene, node);
	if (!it)
		return false;
	::SceneGiVolume& def = it->volume;
	if (attr == "priority") {
		def.priority = uint32_t(std::max(num, 0.0));
	} else if (attr == "spacing") {
		def.spacing = std::max(float(num), 1e-3f);
	} else if (attr == "spacinglow") {
		def.spacingLow = std::max(float(num), 0.0f);
	} else if (attr == "spacinghigh") {
		def.spacingHigh = std::max(float(num), 0.0f);
	} else if (attr == "optlayout") {
		// The optimizer's seed (one undoable step: number = spacing,
		// vec4.xyz = phase; spacing 0 = cleared).
		def.optSpacing = std::max(float(num), 0.0f);
		def.optPhase[0] = v4.x;
		def.optPhase[1] = v4.y;
		def.optPhase[2] = v4.z;
	} else if (attr == "inside") {
		def.insideFade = std::max(float(num), 0.0f);
	} else if (attr == "outside") {
		def.outsideFade = std::max(float(num), 0.0f);
	} else if (attr == "name") {
		const uint32_t id = uint32_t(scene.nodeNames.size());
		scene.nodeNames.push_back(text);
		SetNodeNameIndex(scene, node, id);
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

bool EditorApp::GiVolumeOBB(uint32_t node, glm::vec3& center, glm::mat3& axes,
	glm::vec3& half) const {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return false;
	const Scene& scene = m_world->scene;
	if (!FindGiVolume(scene, node))
		return false;
	if (node >= scene.globalTransform.size())
		return false;
	// Authoring box: center = global translate, half extents = half the
	// global basis lengths, axes = the NORMALIZED basis columns (the
	// node's world rotation — an OBB; the bake rotates the six box
	// planes with it, CvBoxPlanesRotated).
	const glm::mat4& g = scene.globalTransform[node];
	center = glm::vec3(g[3]);
	for (int i = 0; i < 3; ++i) {
		const float len = glm::length(g[i]);
		half[i] = 0.5f * len;
		axes[i] = (len > 1e-6f) ? glm::vec3(g[i]) / len
			: glm::vec3(0.0f);
	}
	return true;
}

bool EditorApp::GiVolumeBox(uint32_t node, glm::vec3& mn, glm::vec3& mx) const {
	// AABB of the (possibly rotated) authoring OBB.
	glm::vec3 c;
	glm::mat3 axes;
	glm::vec3 half;
	if (!GiVolumeOBB(node, c, axes, half))
		return false;
	mn = c;
	mx = c;
	const glm::vec3 ax[3] = { axes[0] * half[0], axes[1] * half[1],
		axes[2] * half[2] };
	for (const glm::vec3& a : ax) {
		mn -= glm::abs(a);
		mx += glm::abs(a);
	}
	return true;
}

bool EditorApp::GiVolumeBoxCorners(uint32_t node,
	glm::vec3 outCorners[8]) const {
	glm::vec3 c;
	glm::mat3 axes;
	glm::vec3 half;
	if (!GiVolumeOBB(node, c, axes, half))
		return false;
	const glm::vec3 ax[3] = { axes[0] * half[0], axes[1] * half[1],
		axes[2] * half[2] };
	for (int i = 0; i < 8; ++i)
		outCorners[i] = c + ((i & 1) ? ax[0] : -ax[0])
			+ ((i & 2) ? ax[1] : -ax[1])
			+ ((i & 4) ? ax[2] : -ax[2]);
	return true;
}

bool EditorApp::ReadNodeCubemapAttr(uint32_t node, const std::string& attr,
	glm::vec4& v4, double& num, std::string& text) const {
	v4 = glm::vec4(0.0f);
	num = 0.0;
	text.clear();
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return false;
	const Scene& scene = m_world->scene;
	auto it = FindCubemap(scene, node);
	if (!it)
		return false;
	const ::SceneCubemap& def = it->cubemap;
	if (attr == "radius") {
		num = double(def.radius);
	} else if (attr == "name") {
		text = getNodeName(scene, int(node));
	} else if (attr == "position") {
		glm::vec3 t, r, s;
		ReadLocalTRS(node, t, r, s);
		v4 = glm::vec4(t, 1.0f);
	} else {
		return false;
	}
	return true;
}

bool EditorApp::WriteNodeCubemapAttr(uint32_t node, const std::string& attr,
	const glm::vec4& v4, double num, const std::string& text) {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return false;
	Scene& scene = m_world->scene;
	auto it = FindCubemap(scene, node);
	if (!it)
		return false;
	::SceneCubemap& def = it->cubemap;
	if (attr == "radius") {
		def.radius = std::max(float(num), 1e-2f);
	} else if (attr == "name") {
		const uint32_t id = uint32_t(scene.nodeNames.size());
		scene.nodeNames.push_back(text);
		SetNodeNameIndex(scene, node, id);
	} else if (attr == "position") {
		glm::vec3 t, r, s;
		ReadLocalTRS(node, t, r, s);
		t = glm::vec3(v4.x, v4.y, v4.z);
		scene.localTransform[node] = LocalMatrixFromTRS(t, r, s);
		markAsChanged(scene, int(node));
		recalculateGlobalTransforms(scene);
	} else {
		return false;
	}
	return true;
}

bool EditorApp::ReadNodeCameraAttr(uint32_t node, const std::string& attr,
	glm::vec4& v4, double& num, std::string& text) const {
	v4 = glm::vec4(0.0f);
	num = 0.0;
	text.clear();
	const ::SceneCamera* C = FindNodeCamera(node);
	if (!C)
		return false;
	if (attr == "projection") {
		num = (C->projection == ::SceneCamera::Projection::Orthographic)
			? 1.0 : 0.0;
	} else if (attr == "fovY") {
		num = double(C->fovYDeg);
	} else if (attr == "orthoHalfH") {
		num = double(C->orthoHalfH);
	} else if (attr == "near") {
		num = double(C->nearClip);
	} else if (attr == "far") {
		num = double(C->farClip);
	} else if (attr == "name") {
		text = C->name;
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

bool EditorApp::WriteNodeCameraAttr(uint32_t node, const std::string& attr,
	const glm::vec4& v4, double num, const std::string& text) {
	if (!m_world)
		return false;
	Scene& scene = m_world->scene;
	auto cit = FindCamera(scene, node);
	if (!cit
		|| cit->camera >= m_world->cameraLocations.size())
		return false;
	const auto& loc = m_world->cameraLocations[cit->camera];
	if (!loc.cluster)
		return false;
	::SceneCamera* C = loc.cluster->cameras.get(loc.handle);
	if (!C)
		return false;
	if (attr == "projection") {
		C->projection = (num >= 0.5)
			? ::SceneCamera::Projection::Orthographic
			: ::SceneCamera::Projection::Perspective;
	} else if (attr == "fovY") {
		C->fovYDeg = std::clamp(float(num), 1.0f, 179.0f);
	} else if (attr == "orthoHalfH") {
		C->orthoHalfH = std::clamp(float(num), 1e-2f, 1e5f);
	} else if (attr == "near") {
		C->nearClip = std::max(float(num), 1e-4f);
		C->farClip = std::max(C->farClip, C->nearClip * 1.1f + 1e-3f);
	} else if (attr == "far") {
		C->farClip = std::max(float(num), C->nearClip * 1.1f + 1e-3f);
	} else if (attr == "name") {
		C->name = text;
		const uint32_t id = uint32_t(scene.nodeNames.size());
		scene.nodeNames.push_back(text);
		SetNodeNameIndex(scene, node, id);
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
	// THE VERB TOAST: a minimal popup message over the view panels'
	// rect — the command's action, at a glance (the "did that register"
	// feedback the gesture-driven UI owes; the status bar is a look-
	// away surface, the stage is where the eyes are).
	ShowVerbToast(c);
	if (c.verb == EditVerb::SetVisibility)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Visibility);
	else if (c.verb == EditVerb::AssignMaterial)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Material);
	else if (c.verb == EditVerb::SetNoLightmap)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::NoLightmap);
	else if (c.verb == EditVerb::SetLightmap)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Lightmap);
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
	else if (c.verb == EditVerb::SetLightmap)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Lightmap);
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
	else if (c.verb == EditVerb::SetLightmap)
		NoteLocalOverrideWritten(c.node, smvk::tour::OverrideAttr::Lightmap);
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
	case EditVerb::SetLightmap:
	case EditVerb::SetDitherCloseup:
	case EditVerb::SetCastsVolumetricShadow:
	case EditVerb::SetStatic:
	case EditVerb::AssignMaterial:
			lastByNode[{ int(c.verb), c.node }] = i;
			break;
		case EditVerb::SetMatAttr:
			lastByAttr[{ int(c.verb), c.matWorld, c.attr }] = i;
			break;
		case EditVerb::SetLightAttr:
			lastByAttr[{ int(c.verb), c.node, c.lightAttr }] = i;
			break;
		case EditVerb::SetGiVolumeAttr:
			lastByAttr[{ int(c.verb), c.node, c.giVolumeAttr }] = i;
			break;
		case EditVerb::SetCubemapAttr:
			lastByAttr[{ int(c.verb), c.node, c.cubemapAttr }] = i;
			break;
		case EditVerb::SetCamAttr:
			lastByAttr[{ int(c.verb), c.node, c.camAttr }] = i;
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
	case EditVerb::SetLightmap:
	case EditVerb::SetDitherCloseup:
	case EditVerb::SetCastsVolumetricShadow:
	case EditVerb::SetStatic:
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
		case EditVerb::SetGiVolumeAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.node, c.giVolumeAttr }] != i;
			break;
		case EditVerb::SetCubemapAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.node, c.cubemapAttr }] != i;
			break;
		case EditVerb::SetCamAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.node, c.camAttr }] != i;
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
	//                              SetLightmap/SetDitherCloseup/
	//                              AssignMaterial
	//   (SetMatAttr, mat, attr)    shared material value edits
	//   (SetLightAttr, node, attr) light property edits
	//   (SetGiVolumeAttr, node, attr)  convex-volume property edits
	//   (SetCamAttr, node, attr)   camera property edits
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
	case EditVerb::SetLightmap:
	case EditVerb::SetDitherCloseup:
	case EditVerb::SetCastsVolumetricShadow:
	case EditVerb::SetStatic:
	case EditVerb::AssignMaterial:
			lastByNode[{ int(c.verb), c.node }] = i;
			break;
		case EditVerb::SetMatAttr:
			lastByAttr[{ int(c.verb), c.matWorld, c.attr }] = i;
			break;
		case EditVerb::SetLightAttr:
			lastByAttr[{ int(c.verb), c.node, c.lightAttr }] = i;
			break;
		case EditVerb::SetGiVolumeAttr:
			lastByAttr[{ int(c.verb), c.node, c.giVolumeAttr }] = i;
			break;
		case EditVerb::SetCubemapAttr:
			lastByAttr[{ int(c.verb), c.node, c.cubemapAttr }] = i;
			break;
		case EditVerb::SetCamAttr:
			lastByAttr[{ int(c.verb), c.node, c.camAttr }] = i;
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
	case EditVerb::SetLightmap:
	case EditVerb::SetDitherCloseup:
	case EditVerb::SetCastsVolumetricShadow:
	case EditVerb::SetStatic:
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
		case EditVerb::SetGiVolumeAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.node, c.giVolumeAttr }] != i;
			break;
		case EditVerb::SetCubemapAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.node, c.cubemapAttr }] != i;
			break;
		case EditVerb::SetCamAttr:
			superseded =
				lastByAttr[{ int(c.verb), c.node, c.camAttr }] != i;
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
	case editor::EditVerb::SetNoLightmap: return "lightmap flag (legacy opt-out)";
	case editor::EditVerb::SetLightmap: return "lightmap flag (opt-in)";
	case editor::EditVerb::SetDitherCloseup: return "dither flag";
	case editor::EditVerb::SetCastsVolumetricShadow: return "volumetric caster mark";
	case editor::EditVerb::SetStatic: return "static mark";
	case editor::EditVerb::SetVisibility: return "visibility";
	case editor::EditVerb::Parent: return "reparent";
	case editor::EditVerb::CreateLight: return "create light";
	case editor::EditVerb::SetLightAttr: return "light edit";
	case editor::EditVerb::CreateTerrain: return "create terrain";
	case editor::EditVerb::TerrainPaint: return "terrain paint";
	case editor::EditVerb::Delete: return "delete";
	case editor::EditVerb::CreateTransform: return "create transform";
	case editor::EditVerb::CreateGiVolume: return "create GI volume";
	case editor::EditVerb::SetGiVolumeAttr: return "GI volume edit";
	case editor::EditVerb::CreateCubemap: return "create cubemap";
	case editor::EditVerb::SetCubemapAttr: return "cubemap edit";
	case editor::EditVerb::CreateCamera: return "create camera";
	case editor::EditVerb::SetCamAttr: return "camera edit";
	case editor::EditVerb::CreateExportSettings: return "create exportSettings";
	case editor::EditVerb::SetExportAttr: return "export setting";
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
	// Cluster id for a create's LIVE parent, NORMALIZED (the six create
	// families below share it): the SCENE-ROOT parent (world node 0 — the
	// synthetic root ABOVE the cluster base, so WorldToCluster(0) reads
	// -1) maps to the CLUSTER ROOT (id 0): the tour id space has no
	// world-0, and the cluster root is the closest valid parent. The
	// un-normalized -1 leaked into the create lines as -p 4294967295
	// (uint32_t(-1)) — those files then failed replay (resolveNode's
	// create-ref check) — 10/6. A lambda for the private member access.
	auto createParentClusterId = [&app](uint32_t liveParent) {
		int pp = app.WorldToCluster(liveParent);
		if (pp < 0 && liveParent == 0)
			pp = 0; // the scene-root parent: the cluster root
		return pp;
	};
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
		if (worldParent < scene.metadata.size()
			&& !scene.metadata[worldParent].uuid.empty())
			return scene.metadata[worldParent].uuid;
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
		// Created-GI-volume nodes own no cluster id either: renames
		// persist as GI-volume-attr lines (-ref addressing).
		const int kv = app.CreatedGiVolumeIndex(c.node);
		if (kv >= 0) {
			out.verb = smvk::tour::Verb::SetGiVolumeAttr;
			out.node = smvk::tour::kCreateGiVolumeRefBase | uint32_t(kv);
			out.text = "name";
			out.name = c.afterText;
			return true;
		}
		// Created-cubemap captures: same treatment.
		const int kc = app.CreatedCubemapIndex(c.node);
		if (kc >= 0) {
			out.verb = smvk::tour::Verb::SetCubemapAttr;
			out.node = smvk::tour::kCreateCubemapRefBase | uint32_t(kc);
			out.text = "name";
			out.name = c.afterText;
			return true;
		}
		// Created cameras: renames persist as cam-attr lines.
		const int kcam = app.CreatedCameraIndex(c.node);
		if (kcam >= 0) {
			out.verb = smvk::tour::Verb::SetCamAttr;
			out.node = smvk::tour::kCreateCameraRefBase | uint32_t(kcam);
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
		// Created-GI-volume nodes: same treatment (GI-volume-attr triples).
		const int kv = app.CreatedGiVolumeIndex(c.node);
		if (kv >= 0) {
			out.verb = smvk::tour::Verb::SetGiVolumeAttr;
			out.node = smvk::tour::kCreateGiVolumeRefBase | uint32_t(kv);
			out.text = (c.verb == EditVerb::Translate) ? "position" : "rotation";
			out.vec4 = glm::vec4(c.afterVec, 1.0f);
			return true;
		}
		// Created-cubemap captures: position channel only (rotation is
		// meaningless for a probe, but the lowering keeps parity).
		const int kc = app.CreatedCubemapIndex(c.node);
		if (kc >= 0) {
			out.verb = smvk::tour::Verb::SetCubemapAttr;
			out.node = smvk::tour::kCreateCubemapRefBase | uint32_t(kc);
			out.text = (c.verb == EditVerb::Translate) ? "position" : "rotation";
			out.vec4 = glm::vec4(c.afterVec, 1.0f);
			return true;
		}
		// Created cameras: moves/aims persist as cam-attr triples so
		// gizmo edits survive (their cluster ids don't exist until
		// replay, exactly like created lights).
		const int kcam = app.CreatedCameraIndex(c.node);
		if (kcam >= 0) {
			out.verb = smvk::tour::Verb::SetCamAttr;
			out.node = smvk::tour::kCreateCameraRefBase | uint32_t(kcam);
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
		// Created-GI-volume size edits ride the size channel (-ref).
		const int kv = app.CreatedGiVolumeIndex(c.node);
		if (kv >= 0) {
			out.verb = smvk::tour::Verb::SetGiVolumeAttr;
			out.node = smvk::tour::kCreateGiVolumeRefBase | uint32_t(kv);
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
	case EditVerb::SetLightmap:
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::SetLightmap;
		out.flag = c.afterFlag;
		return true;
	case EditVerb::SetDitherCloseup:
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::SetDitherCloseup;
		out.flag = c.afterFlag;
		return true;
	case EditVerb::SetCastsVolumetricShadow:
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::SetCastsVolumetricShadow;
		out.flag = c.afterFlag;
		return true;
	case EditVerb::SetStatic:
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::SetStatic;
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
		int pn = app.WorldToCluster(c.node2);
		if (pn < 0) {
			if (c.node2 == 0)
				pn = 0; // world root: the primary cluster's root (id 0)
			else {
				// Editor-created new parent: symbolic -p emission (see
				// the CreateLight case) — the reparent survives saves
				// instead of silently vanishing from the cooked scene.
				if (parentRefFor(c.node2).empty()
					|| !EditorApp::ParentResolvesInTour(app, c.node2))
					return false;
				pn = 0; // numeric fallback unused (writer prefers -pStr)
			}
		}
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
		// Parent = the LIVE parent (reparents ride the create line
		// itself — see CreateEmitParent; Parent verbs on created
		// children skip silently at save).
		const uint32_t liveParent = EditorApp::CreateEmitParent(app, c);
		int pp = createParentClusterId(liveParent);
		if (pp < 0 && liveParent != 0) {
			// Editor-created parent (no cluster id until replay): emit
			// the SYMBOLIC -p form (name/path; the parent's own create
			// lands earlier in file order, so replay resolves it)
			// instead of dropping the create — the drop desynced the
			// family -ref spaces ("setLightAttr ref out of range").
			if (!parentRefFor(liveParent).empty()
				&& EditorApp::ParentResolvesInTour(app, liveParent))
				pp = 0; // numeric fallback unused (writer prefers -pStr)
			else
				return false;
		}
		const glm::vec3 t = glm::vec3(
			app.m_world->scene.localTransform[created][3]);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = (L->type == ::SceneLight::Type::Spot)
			? "spotLight" : "pointLight";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(liveParent);
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
		int pp = 0;
		// Parent = the LIVE parent (reparents ride the create line
		// itself — see CreateEmitParent; Parent verbs on created
		// children skip silently at save).
		const uint32_t liveParent = EditorApp::CreateEmitParent(app, c);
		pp = createParentClusterId(liveParent);
		if (pp < 0 && liveParent != 0) {
			// Editor-created parent: symbolic -p emission (see the
			// CreateLight case) — never drop the terrain silently.
			if (!parentRefFor(liveParent).empty()
				&& EditorApp::ParentResolvesInTour(app, liveParent))
				pp = 0; // numeric fallback unused (writer prefers -pStr)
			else
				return false;
		}
		const glm::vec3 t = glm::vec3(
			app.m_world->scene.localTransform[created][3]);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = "terrain";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(liveParent);
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
		// Parent = the LIVE parent (reparents ride the create line
		// itself — see CreateEmitParent).
		const uint32_t liveParent = EditorApp::CreateEmitParent(app, c);
		int pp = createParentClusterId(liveParent);
		if (pp < 0 && liveParent != 0) {
			// Editor-created parent: symbolic -p emission (see the
			// CreateLight case) — never drop the transform silently.
			if (!parentRefFor(liveParent).empty()
				&& EditorApp::ParentResolvesInTour(app, liveParent))
				pp = 0; // numeric fallback unused (writer prefers -pStr)
			else
				return false;
		}
		glm::vec3 t, r, s;
		app.ReadLocalTRS(created, t, r, s);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = "transform";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(liveParent);
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
		} else if (c.lightAttr == "name" || c.lightAttr == "cookie") {
			// String attrs ride the name slot (cookie = the gobo uri).
			out.name = c.afterText;
		} else if (c.lightAttr == "static" || c.lightAttr == "shadow") {
			out.flag = c.afterFlag;
		} else {
			// Scalars (intensity/range/inner/outer/function/
			// functionSpeed/functionScale) ride the number slot.
			out.number = c.number;
		}
		return true;
	}
	case EditVerb::CreateGiVolume: {
		// Emit the LIVE create state: the node may have moved/scaled
		// and its params may have been attr-edited after creation
		// (those later SetGiVolumeAttr commands re-emit harmlessly and
		// idempotently — the light precedent).
		const uint32_t created = c.node;
		if (created == UINT32_MAX
			|| created >= app.m_world->scene.hierarchy.size())
			return false;
		auto cvit = FindGiVolume(app.m_world->scene, created);
		if (!cvit)
			return false; // detached (tombstoned): nothing to persist
		if (app.m_deletedNodes.contains(created))
			return false;
		// Parent = the LIVE parent (see CreateEmitParent).
		const uint32_t liveParent = EditorApp::CreateEmitParent(app, c);
		int pp = createParentClusterId(liveParent);
		if (pp < 0 && liveParent != 0) {
			// Editor-created parent: symbolic -p emission (see the
			// CreateLight case) — never drop the GI volume silently.
			if (!parentRefFor(liveParent).empty()
				&& EditorApp::ParentResolvesInTour(app, liveParent))
				pp = 0; // numeric fallback unused (writer prefers -pStr)
			else
				return false;
		}
		glm::vec3 t, r, s;
		app.ReadLocalTRS(created, t, r, s);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = "givolume";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(liveParent);
		out.text = getNodeName(app.m_world->scene, int(created));
		out.vec = t;
		out.svec = glm::vec3(std::max(s.x, 1e-3f),
			std::max(s.y, 1e-3f), std::max(s.z, 1e-3f));
		out.vec4 = glm::vec4(float(cvit->volume.priority),
			cvit->volume.spacing, cvit->volume.insideFade,
			cvit->volume.outsideFade);
		return true;
	}
	case EditVerb::SetGiVolumeAttr: {
		const int k = app.CreatedGiVolumeIndex(c.node);
		if (k >= 0) {
			out.node = smvk::tour::kCreateGiVolumeRefBase | uint32_t(k);
		} else if (!needNode()) {
			return false;
		}
		out.verb = smvk::tour::Verb::SetGiVolumeAttr;
		out.text = c.giVolumeAttr;
		if (c.giVolumeAttr == "position" || c.giVolumeAttr == "rotation"
			|| c.giVolumeAttr == "size") {
			out.vec4 = c.vec4;
		} else if (c.giVolumeAttr == "name") {
			out.name = c.afterText;
		} else if (c.giVolumeAttr == "optlayout") {
			// The seed rides BOTH channels: number = spacing,
			// vec4.xyz = phase (one verb, one undo step).
			out.vec4 = c.vec4;
			out.number = c.number;
		} else {
			out.number = c.number;
		}
		return true;
	}
	case EditVerb::CreateCubemap: {
		// Emit the LIVE create state: the node may have moved and its
		// radius may have been attr-edited after creation (those later
		// SetCubemapAttr commands re-emit harmlessly and idempotently).
		const uint32_t created = c.node;
		if (created == UINT32_MAX
			|| created >= app.m_world->scene.hierarchy.size())
			return false;
		auto cmit = FindCubemap(app.m_world->scene, created);
		if (!cmit)
			return false; // detached (tombstoned): nothing to persist
		if (app.m_deletedNodes.contains(created))
			return false;
		// Parent = the LIVE parent (see CreateEmitParent).
		const uint32_t liveParent = EditorApp::CreateEmitParent(app, c);
		int pp = createParentClusterId(liveParent);
		if (pp < 0 && liveParent != 0) {
			// Editor-created parent (e.g. the auto-selected previous
			// create — DraftCreateCubemap parents under the selection):
			// emit the SYMBOLIC -p form instead of dropping the create.
			// The historical drop is what produced orphaned
			// "setCubemapAttr -ref k" lines and the cook failure
			// "tour: setCubemapAttr ref out of range: 0" (GiBaker and
			// PakCooker both abort on it).
			if (!parentRefFor(liveParent).empty()
				&& EditorApp::ParentResolvesInTour(app, liveParent))
				pp = 0; // numeric fallback unused (writer prefers -pStr)
			else
				return false;
		}
		glm::vec3 t, r, s;
		app.ReadLocalTRS(created, t, r, s);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = "cubemap";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(liveParent);
		out.text = getNodeName(app.m_world->scene, int(created));
		out.vec = t;
		out.number = double(cmit->cubemap.radius);
		return true;
	}
	case EditVerb::SetCubemapAttr: {
		const int k = app.CreatedCubemapIndex(c.node);
		if (k >= 0) {
			out.node = smvk::tour::kCreateCubemapRefBase | uint32_t(k);
		} else if (!needNode()) {
			return false;
		}
		out.verb = smvk::tour::Verb::SetCubemapAttr;
		out.text = c.cubemapAttr;
		if (c.cubemapAttr == "position") {
			out.vec4 = c.vec4;
		} else if (c.cubemapAttr == "name") {
			out.name = c.afterText;
		} else {
			out.number = c.number;
		}
		return true;
	}
	case EditVerb::CreateCamera: {
		// Emit the LIVE create state: the node may have moved and its
		// lens may have been attr-edited after creation (those later
		// SetCamAttr commands re-emit harmlessly and idempotently).
		const uint32_t created = c.node;
		if (created == UINT32_MAX
			|| created >= app.m_world->scene.hierarchy.size())
			return false;
		const ::SceneCamera* C = app.FindNodeCamera(created);
		if (!C)
			return false; // detached (tombstoned): nothing to persist
		if (app.m_deletedNodes.contains(created))
			return false;
		// Parent = the LIVE parent (see CreateEmitParent).
		const uint32_t liveParent = EditorApp::CreateEmitParent(app, c);
		int pp = createParentClusterId(liveParent);
		if (pp < 0 && liveParent != 0) {
			// Editor-created parent: symbolic -p emission (see the
			// CreateLight case) — never drop the camera silently.
			if (!parentRefFor(liveParent).empty()
				&& EditorApp::ParentResolvesInTour(app, liveParent))
				pp = 0; // numeric fallback unused (writer prefers -pStr)
			else
				return false;
		}
		glm::vec3 t, r, s;
		app.ReadLocalTRS(created, t, r, s);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = "camera";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(liveParent);
		out.text = getNodeName(app.m_world->scene, int(created));
		out.vec = t;
		out.svec = r;
		out.vec4 = glm::vec4(
			C->projection == ::SceneCamera::Projection::Orthographic
				? 1.0f : 0.0f,
			C->fovYDeg, C->nearClip, C->farClip);
		out.number = double(C->orthoHalfH);
		return true;
	}
	case EditVerb::SetCamAttr: {
		const int k = app.CreatedCameraIndex(c.node);
		if (k >= 0) {
			out.node = smvk::tour::kCreateCameraRefBase | uint32_t(k);
		} else if (!needNode()) {
			return false;
		}
		out.verb = smvk::tour::Verb::SetCamAttr;
		out.text = c.camAttr;
		if (c.camAttr == "position" || c.camAttr == "rotation") {
			out.vec4 = c.vec4;
		} else if (c.camAttr == "name") {
			out.name = c.afterText;
		} else {
			out.number = c.number;
		}
		return true;
	}
	case EditVerb::CreateExportSettings: {
		// The scene's export config node: a plain createNode -at
		// exportSettings line (parent + name; identity transform). The
		// attr payload rides the following SetExportAttr commands
		// (live-state emission there, like the GI-volume family).
		const uint32_t created = c.node;
		if (created == UINT32_MAX
			|| created >= app.m_world->scene.hierarchy.size())
			return false;
		if (!FindExport(app.m_world->scene, created))
			return false; // detached (tombstoned): nothing to persist
		if (app.m_deletedNodes.contains(created))
			return false;
		// Parent = the LIVE parent (see CreateEmitParent).
		const uint32_t liveParent = EditorApp::CreateEmitParent(app, c);
		int pp = createParentClusterId(liveParent);
		if (pp < 0 && liveParent != 0) {
			// Editor-created parent: symbolic -p emission (see the
			// CreateLight case) — never drop the node silently.
			if (!parentRefFor(liveParent).empty()
				&& EditorApp::ParentResolvesInTour(app, liveParent))
				pp = 0; // numeric fallback unused (writer prefers -pStr)
			else
				return false;
		}
		glm::vec3 t, r, s;
		app.ReadLocalTRS(created, t, r, s);
		out.verb = smvk::tour::Verb::CreateNode;
		out.nodeType = "exportSettings";
		out.node = uint32_t(pp);
		out.parentRef = parentRefFor(liveParent);
		out.text = getNodeName(app.m_world->scene, int(created));
		out.vec = t;
		return true;
	}
	case EditVerb::SetExportAttr: {
		const int k = app.CreatedExportIndex(c.node);
		if (k >= 0) {
			out.node = smvk::tour::kCreateExportRefBase | uint32_t(k);
		} else if (!needNode()) {
			return false;
		}
		out.verb = smvk::tour::Verb::SetExportAttr;
		out.text = c.exportAttr;
		if (c.exportAttr == "meshopt" || c.exportAttr == "lods"
			|| c.exportAttr == "lightmap" || c.exportAttr == "cubemap"
			|| c.exportAttr == "cubemapOctree"
			|| c.exportAttr == "meshlets" || c.exportAttr == "giVolumes"
			|| c.exportAttr == "giWorld" || c.exportAttr == "autoKtx"
			|| c.exportAttr == "ignoreVfs") {
			out.flag = c.afterFlag; // bool attrs ride the flag slot
		} else if (c.exportAttr == "lodRatios"
			|| c.exportAttr == "lmExcludeKw") {
			out.name = c.afterText; // string attrs ride the name slot
		} else {
			out.number = c.number; // scalars ride the number slot
		}
		return true;
	}
	case EditVerb::Delete: {
		// Cluster-backed nodes persist as a first-class Maya delete.
		// Addressing: uuid (file-stable identity, assigned for every
		// delete target by the SaveTourTo pre-pass), name (secondary,
		// via needNode's verify-name side effect), numeric id as the
		// legacy fallback/disambiguator. Editor-created nodes (created
		// lights, GI volumes, terrain groups/tiles — no cluster id) are
		// never written: their creates are dropped by the live-state
		// emission above, removing them from the file entirely.
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::Delete;
		const std::string& u = GetNodeUuid(app.m_world->scene, c.node);
		if (!u.empty())
			out.uuid = u;
		return true;
	}
	}
	return false;
}

std::string EditorApp::TourPath() const {
	if (m_gltfPath.empty())
		return {};
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
	// Same doc as a real save, but the target stays untouched.
	// The APPDATA autosave slot is external by design (allowExternal).
	return SaveTourTo(path, false, true);
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

bool EditorApp::SaveTourTo(const std::string& tp, bool becomeTarget,
	bool allowExternal) {
	if (tp.empty() || !m_world)
		return false;
	// Project workflow: scenes live inside the project folder. User
	// saves outside it are refused (Save As into content/scenes/
	// instead); internal temp snapshots (cook/reload) and the APPDATA
	// autosave slot pass allowExternal.
	if (!allowExternal && !m_projectRoot.empty() && !InProjectDir(tp)) {
		Log("Tour save refused: '" + tp + "' is outside the project — "
			"scenes live inside '" + m_projectRoot + "' (Save As into "
			"content/scenes/).");
		return false;
	}
	{
		// Save-as may point at a directory that does not exist yet.
		std::error_code ec;
		const std::filesystem::path parent =
			std::filesystem::path(tp).parent_path();
		if (!parent.empty())
			std::filesystem::create_directories(parent, ec);
	}
	smvk::tour::Doc doc;
	// THE DEFINITION IS SELF-CONTAINED: no `source` line, no
	// fingerprint — the scene's glTF rides as reference #0 (a ROOT
	// reference), bound identically by the editor and the cooker.
	// m_gltfPath/m_gltfHostPath stay editor-side identity/picking
	// state only. (VFS paths use forward slashes — the cooker's
	// resolver expects POSIX separators.)
	doc.source.clear();
	doc.fingerprint.clear();
	if (!m_gltfHostPath.empty()) {
		std::string posix = m_gltfHostPath;
		for (char& c : posix)
			if (c == '\\')
				c = '/';
		// Project workflow: the root source is the project-relative
		// asset path (never absolute). Project-less sessions keep the
		// absolute posix form (no root to relativize against).
		const std::string rel = ProjectRelativePath(m_gltfHostPath);
		if (!rel.empty())
			posix = rel;
		// ROOT = THE DERIVED .GEOMETRY DATA BLOB (the preprocessed scene
		// graph — see asset_export.h): the tour references the ASSET, not
		// the glTF; the bind imports the sibling source glTF (the
		// derivation wrote them next to each other; node ids are
		// identical — the same deterministic import). GUID = the geometry
		// entry's manifest GUID (stable across reimports). Falls back to
		// the glTF when no .geometry was derived yet (a pre-rename
		// derivation's legacy ".mesh" blob still binds — the loader takes
		// both — but the next re-import derives ".geometry" and the swap
		// below re-targets it).
		// The root GUID starts as the SOURCE CONTENT identity (the same
		// one the derived .meta records — see ImportMeshArtifactFor /
		// asset_export.h): project sessions and from-zero re-imports
		// agree on ONE identity (the project DB's guid is separate
		// bookkeeping, not the artifact identity).
		std::string rootGuid =
			smvk::assets::SourceAssetGuid(m_gltfHostPath);
		{
			std::error_code mec;
			const std::filesystem::path gltfStem =
				std::filesystem::path(m_gltfHostPath).stem();
			const std::string geometrySibling =
				(std::filesystem::path(m_gltfHostPath).parent_path()
					/ (gltfStem.string() + ".geometry")).generic_string();
		// Reference the .geometry ASSET only when its manifest GUID resolves:
		// source and GUID must stay a CONSISTENT pair. Switching the source
		// to .geometry while keeping the glTF GUID (manifest entry missing)
		// produced a mismatch the heal loop below rewrote EVERY save (the
		// "Tour save: reference ..." log spam — SaveTourTo runs per edit,
		// per snapshot, per close), re-derived to .geometry again next save.
		if (std::filesystem::is_regular_file(geometrySibling, mec)) {
			const std::string geometryGuid =
				DerivedAssetGuid(geometrySibling);
			if (!geometryGuid.empty()) {
				posix = (std::filesystem::path(posix).parent_path()
					/ (gltfStem.string() + ".geometry")).generic_string();
				rootGuid = geometryGuid;
			}
		}
		}
		smvk::tour::ReferenceCmd rootRef{};
		rootRef.root = true;
		rootRef.parent = 0;
		rootRef.source = posix;
		rootRef.guid = rootGuid;
		rootRef.name = std::filesystem::path(posix).stem().string();
		// Keep the root reference leading the definition (fresh source).
		if (!m_references.empty() && m_references[0].root)
			m_references[0] = rootRef;
		else
			m_references.insert(m_references.begin(), rootRef);
	}
	// UUID pre-pass: nodes addressed by uuid or symbolic -p need a
	// file-stable identity recorded. Assign missing ones NOW into the
	// world scene (deterministic from here: already-assigned ids are
	// kept). Covers Delete targets AND symbolic parent targets (a
	// parent addressed by uuid must exist in the file) — named parents
	// resolve by name and need nothing.
	Scene& scene = m_world->scene;
	auto held = [&](const std::string& hex, uint32_t except) {
		for (uint32_t n = 0; n < scene.metadata.size(); ++n) {
			const std::string& u = scene.metadata[n].uuid;
			if (!u.empty() && u == hex && n != except)
				return true;
		}
		return false;
	};
	auto ensureUuid = [&](uint32_t wnode) {
		if (wnode >= scene.hierarchy.size())
			return;
		// Named nodes ride names/paths in the symbolic -p slot — only
		// UNNAMED targets need a recorded uuid to stay addressable.
		if (!getNodeName(scene, int(wnode)).empty())
			return;
		if ((!GetNodeUuid(scene, wnode).empty()))
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
		SetNodeUuidRaw(scene, wnode, hex);
	};
	for (size_t i : CoalescedSaveIndices()) {
		const EditCmd& c = m_history[i];
		if (c.verb == EditVerb::Delete) {
			const int cn = WorldToCluster(c.node);
			if (cn < 0)
				continue; // editor-created: dropped via live-state emission
			const std::string& u = GetNodeUuid(scene, c.node);
			if (!u.empty() && !held(u, c.node))
				continue; // file-stable identity already in place
			ensureUuid(c.node);
			continue;
		}
		// Symbolic parents: unnamed targets need recorded uuids so
		// the -p slot can carry them (named ones ride names/paths).
		// Creates emit their LIVE parent (see CreateEmitParent), so
		// the uuid pass follows the live hierarchy, not the recorded
		// creation-time parent.
		if (c.verb == EditVerb::Parent) {
			ensureUuid(c.node);
			ensureUuid(c.node2);
		}
		else if (c.verb == EditVerb::CreateLight
			|| c.verb == EditVerb::CreateTerrain
			|| c.verb == EditVerb::CreateTransform
			|| c.verb == EditVerb::CreateGiVolume
			|| c.verb == EditVerb::CreateCubemap
			|| c.verb == EditVerb::CreateCamera) {
			ensureUuid(CreateEmitParent(*this, c));
		}
	}
	// Emit the whole identity map (cluster-backed, node-id order —
	// deterministic files): these feed the delete lines AND survive
	// reloads. Without this, a reload drops the identities and the
	// next save mints fresh ones (uuid churn: -uuid links would never
	// match twice).
	std::vector<uint32_t> uuidNodes;
	for (uint32_t wnode = 0; wnode < scene.metadata.size(); ++wnode) {
		const std::string& hex = scene.metadata[wnode].uuid;
		if (hex.empty())
			continue;
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
		u.uuid = GetNodeUuid(scene, wnode);
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
		// Parent verbs on editor-created children never persist (see
		// IsCreatedChild): when the child's create emits, the create
		// line carries the live parent; when it doesn't, the child
		// isn't in the file at all. Skip silently either way (no
		// "unmapped command" spam — the outcome is structural, not a
		// translation miss).
		if (m_history[i].verb == EditVerb::Parent
			&& IsCreatedChild(*this, m_history[i].node))
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
	// edits from the file. EXCEPT family-ref attr orphans pointing past
	// the create set this save emits (setLightAttr/setGiVolumeAttr/
	// setCubemapAttr/setCamAttr/terrainPaint with -ref k >= the
	// family's emitted creates): a create set is fixed per file, so a
	// past-the-end -ref can never resolve again — re-emitting it only
	// poisons every cook with "ref out of range" (the strict replay
	// aborts). Drop those with a log line instead.
	{
		const auto familyRefOutOfRange =
			[&](const smvk::tour::Command& tc) {
				using smvk::tour::Verb;
				auto baseOf = [](Verb v) -> uint32_t {
					switch (v) {
					case Verb::SetLightAttr:
						return smvk::tour::kCreateLightRefBase;
					case Verb::SetGiVolumeAttr:
						return smvk::tour::kCreateGiVolumeRefBase;
					case Verb::SetCubemapAttr:
						return smvk::tour::kCreateCubemapRefBase;
					case Verb::SetCamAttr:
						return smvk::tour::kCreateCameraRefBase;
					case Verb::TerrainPaint:
						return smvk::tour::kCreateTerrainRefBase;
					default:
						return 0;
					}
				};
				const uint32_t base = baseOf(tc.verb);
				if (base == 0 || (tc.node & base) == 0)
					return false; // not a family-ref line (-n addressing)
				const uint32_t k = tc.node & ~base;
				uint32_t emitted = 0;
				for (size_t i = 0; i < m_historyCursor
					&& i < m_history.size(); ++i) {
					const EditVerb ev = m_history[i].verb;
					if ((tc.verb == Verb::SetLightAttr
							&& ev == EditVerb::CreateLight)
						|| (tc.verb == Verb::SetGiVolumeAttr
							&& ev == EditVerb::CreateGiVolume)
						|| (tc.verb == Verb::SetCubemapAttr
							&& ev == EditVerb::CreateCubemap)
						|| (tc.verb == Verb::SetCamAttr
							&& ev == EditVerb::CreateCamera)
						|| (tc.verb == Verb::TerrainPaint
							&& ev == EditVerb::CreateTerrain)) {
						if (CreateCmdEmitsInTour(*this, m_history[i]))
							++emitted;
					}
				}
				return k >= emitted;
			};
		for (size_t oi = 0; oi < m_orphanTourCmds.size();) {
			if (familyRefOutOfRange(m_orphanTourCmds[oi])) {
				Log("tour: dropping orphan '" + smvk::tour::Describe(
						m_orphanTourCmds[oi])
					+ "' (family -ref past this save's create set — it "
					"can never resolve).");
				m_orphanTourCmds.erase(m_orphanTourCmds.begin()
					+ ptrdiff_t(oi));
				continue;
			}
			++oi;
		}
	}
	doc.commands.insert(doc.commands.end(),
		m_orphanTourCmds.begin(), m_orphanTourCmds.end());
	SaveOverridesToDoc(doc);
	SaveEnvToDoc(doc); // editor IBL / env nodes — never applied at cook
	SaveDefaultCameraToDoc(doc); // world default camera (typed ref field)
	SavePlacementToDoc(doc); // scatter graphs — editor-only, never cooked
	SaveImagesToDoc(doc); // image definitions — editor-only, never cooked
	// (Texture relinks no longer ride the tour: they are INPUT PLUGS on
	// the source assets' .meta sidecars — the importer applies them.)
	// Save-time healing (project workflow): reference -source slots
	// carry project-relative posix paths, never absolute ones. A
	// recorded absolute/out-of-project source whose GUID resolves to
	// an in-project asset is rewritten (old tours heal on save); the
	// live list updates too so the panel shows the stored form, and
	// the rewrite is stable (no log spam on later saves).
	// Unresolvable sources persist verbatim (the bind reports them).
	// The ROOT entry skips: its source+GUID pair is freshly (and
	// consistently) derived above, so it can never be stale — healing
	// it would only fight the derivation.
	for (auto& r : m_references) {
		if (r.root)
			continue; // freshly derived above: never stale
		if (r.guid.empty() || m_projectRoot.empty()
			|| !m_assetDb.IsOpen())
			continue;
		std::string byGuid;
		if (!m_assetDb.PathForGuid(r.guid, byGuid) || byGuid.empty())
			continue;
		const std::string rel = ProjectRelativePath(byGuid);
		if (!rel.empty() && rel != r.source) {
			Log("Tour save: reference '" + r.source + "' -> '" + rel
				+ "' (uuid " + r.guid + ").");
			r.source = rel;
		}
	}
	doc.references = m_references; // scene references (loader binds them)
	// Material-asset POOL imports (ordered; see MaterialAssetCmd): the
	// records mirror the session's pool creations so reload/cook rebuild
	// the identical pool — the tour's assignMaterial/-m indices recorded
	// against this pool resolve on both sides.
	doc.materialAssets = m_materialAssets;
	// Texture-asset POOL imports (ordered; see TextureAssetCmd): the
	// session's cookie/.texture pool imports, replayed by reload/cook
	// BEFORE the material records so refs resolve identically.
	doc.textureAssets = m_textureAssets;
	// GI-asset bindings (the scene's HARD baked-GI identification; see
	// GiAssetCmd): the session mirror rides the doc — only the bake
	// owner's stamps update it (the cook / Bake GI flows), reload
	// round-trips it verbatim (the orphan-command contract: a reload
	// must never erase records).
	doc.giAssets = m_giAssets;
	// Cook/export knobs ride the scene's exportSettings NODE (emitted by
	// the history replay above as createNode -at exportSettings +
	// setExportAttr lines); outPak/contentDir stay session-only — a
	// checked-in .tour must not pin a local output path.
	std::string err;
	if (!smvk::tour::SaveTour(tp, doc, err)) {
		Log("tour save failed: " + err);
		return false;
	}
	if (!becomeTarget)
		return true; // snapshot (cook temp tour): live target untouched
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
		const MaterialRow* row =
			FindMaterial(app.m_world->scene, node);
		out.beforeMaterial = row ? row->material : UINT32_MAX;
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
	case smvk::tour::Verb::SetLightmap:
		if (!needNode())
			return false;
		out.verb = EditVerb::SetLightmap;
		out.afterFlag = tc.flag;
		out.beforeFlag = node < app.m_world->scene.lightmap.size() &&
			app.m_world->scene.lightmap[node] != 0;
		return true;
	case smvk::tour::Verb::SetDitherCloseup:
		if (!needNode())
			return false;
		out.verb = EditVerb::SetDitherCloseup;
		out.afterFlag = tc.flag;
		out.beforeFlag = node < app.m_world->scene.ditherCloseup.size() &&
			app.m_world->scene.ditherCloseup[node] != 0;
		return true;
	case smvk::tour::Verb::SetCastsVolumetricShadow:
		if (!needNode())
			return false;
		out.verb = EditVerb::SetCastsVolumetricShadow;
		out.afterFlag = tc.flag;
		out.beforeFlag =
			IsCastsVolumetricShadowNode(app.m_world->scene, int(node));
		return true;
	case smvk::tour::Verb::SetVisibility:
		if (!needNode())
			return false;
		out.verb = EditVerb::SetVisibility;
		out.afterFlag = tc.flag;
		out.beforeFlag = node < app.m_world->scene.hidden.size() &&
			app.m_world->scene.hidden[node] != 0;
		return true;
	case smvk::tour::Verb::SetStatic:
		if (!needNode())
			return false;
		out.verb = EditVerb::SetStatic;
		out.afterFlag = tc.flag;
		out.beforeFlag = node < app.m_world->scene.staticRoot.size() &&
			app.m_world->scene.staticRoot[node] != 0;
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
		if (tn == "givolume") {
			out.verb = EditVerb::CreateGiVolume;
			out.node = UINT32_MAX;
			out.node2 = wp;
			out.afterText = tc.text;
			out.afterVec = tc.vec;
			out.svec = tc.svec;
			out.giVolumePriority = uint32_t(std::max(tc.vec4.x, 0.0f));
			out.giVolumeSpacing = std::max(tc.vec4.y, 1e-3f);
			out.giVolumeInsideFade = std::max(tc.vec4.z, 0.0f);
			out.giVolumeOutsideFade = std::max(tc.vec4.w, 0.0f);
			return true;
		}
		if (tn == "cubemap") {
			out.verb = EditVerb::CreateCubemap;
			out.node = UINT32_MAX;
			out.node2 = wp;
			out.afterText = tc.text;
			out.afterVec = tc.vec;
			out.cubemapRadius = std::max(float(tc.number), 1e-2f);
			return true;
		}
		if (tn == "camera") {
			out.verb = EditVerb::CreateCamera;
			out.node = UINT32_MAX;
			out.node2 = wp;
			out.afterText = tc.text;
			out.afterVec = tc.vec;
			out.camRot = tc.svec;
			out.camProjection = (tc.vec4.x >= 0.5f) ? 1u : 0u;
			out.camFovY = tc.vec4.y;
			out.camNear = tc.vec4.z;
			out.camFar = tc.vec4.w;
			out.camOrthoHalfH = float(tc.number);
			return true;
		}
		if (tn == "exportSettings") {
			out.verb = EditVerb::CreateExportSettings;
			out.node = UINT32_MAX; // assigned at forward execution
			out.node2 = wp;        // parent (world)
			out.afterText = tc.text;
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
	case smvk::tour::Verb::CreateGiVolume: {
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
		out.verb = EditVerb::CreateGiVolume;
		out.node = UINT32_MAX; // assigned at forward execution
		out.node2 = wp;        // parent (world)
		out.afterText = tc.text;
		out.afterVec = tc.vec;
		out.svec = tc.svec;
		out.giVolumePriority = uint32_t(std::max(tc.vec4.x, 0.0f));
		out.giVolumeSpacing = std::max(tc.vec4.y, 1e-3f);
		out.giVolumeInsideFade = std::max(tc.vec4.z, 0.0f);
		out.giVolumeOutsideFade = std::max(tc.vec4.w, 0.0f);
		return true;
	}
	case smvk::tour::Verb::CreateCubemap: {
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
		out.verb = EditVerb::CreateCubemap;
		out.node = UINT32_MAX; // assigned at forward execution
		out.node2 = wp;        // parent (world)
		out.afterText = tc.text;
		out.afterVec = tc.vec;
		out.cubemapRadius = std::max(float(tc.number), 1e-2f);
		return true;
	}
	case smvk::tour::Verb::CreateCamera: {
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
		out.verb = EditVerb::CreateCamera;
		out.node = UINT32_MAX; // assigned at forward execution
		out.node2 = wp;        // parent (world)
		out.afterText = tc.text;
		out.afterVec = tc.vec;
		out.camRot = tc.svec;
		out.camProjection = (tc.vec4.x >= 0.5f) ? 1u : 0u;
		out.camFovY = tc.vec4.y;
		out.camNear = tc.vec4.z;
		out.camFar = tc.vec4.w;
		out.camOrthoHalfH = float(tc.number);
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
		} else if (tc.text == "name" || tc.text == "cookie") {
			out.afterText = tc.name;
		} else if (tc.text == "static" || tc.text == "shadow") {
			out.afterFlag = tc.flag;
		} else {
			out.number = tc.number;
		}
		return true;
	}
	case smvk::tour::Verb::SetGiVolumeAttr: {
		uint32_t target = UINT32_MAX;
		if ((tc.node & smvk::tour::kCreateGiVolumeRefBase) != 0) {
			const uint32_t k = tc.node & ~smvk::tour::kCreateGiVolumeRefBase;
			if (k >= app.m_tourCreatedGiVolumeNodes.size())
				return false;
			target = app.m_tourCreatedGiVolumeNodes[k];
		} else {
			target = app.ClusterToWorld(tc.node);
		}
		if (target == UINT32_MAX
			|| target >= app.m_world->scene.hierarchy.size())
			return false;
		if (!(FindGiVolume(app.m_world->scene, target) != nullptr))
			return false; // not a GI volume: orphan, preserved verbatim
		out.verb = EditVerb::SetGiVolumeAttr;
		out.node = target;
		out.giVolumeAttr = tc.text;
		// Before-state from the live world (exact undo).
		if (!app.ReadNodeGiVolumeAttr(target, tc.text, out.beforeVec4,
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
	case smvk::tour::Verb::SetCubemapAttr: {
		uint32_t target = UINT32_MAX;
		if ((tc.node & smvk::tour::kCreateCubemapRefBase) != 0) {
			const uint32_t k = tc.node & ~smvk::tour::kCreateCubemapRefBase;
			if (k >= app.m_tourCreatedCubemapNodes.size())
				return false;
			target = app.m_tourCreatedCubemapNodes[k];
		} else {
			target = app.ClusterToWorld(tc.node);
		}
		if (target == UINT32_MAX
			|| target >= app.m_world->scene.hierarchy.size())
			return false;
		if (!(FindCubemap(app.m_world->scene, target) != nullptr))
			return false; // not a capture: orphan, preserved verbatim
		out.verb = EditVerb::SetCubemapAttr;
		out.node = target;
		out.cubemapAttr = tc.text;
		// Before-state from the live world (exact undo).
		if (!app.ReadNodeCubemapAttr(target, tc.text, out.beforeVec4,
				out.beforeNum, out.beforeText))
			return false;
		if (tc.text == "position") {
			out.vec4 = tc.vec4;
		} else if (tc.text == "name") {
			out.afterText = tc.name;
		} else {
			out.number = tc.number;
		}
		return true;
	}
	case smvk::tour::Verb::SetCamAttr: {
		uint32_t target = UINT32_MAX;
		if ((tc.node & smvk::tour::kCreateCameraRefBase) != 0) {
			const uint32_t k = tc.node & ~smvk::tour::kCreateCameraRefBase;
			if (k >= app.m_tourCreatedCameraNodes.size())
				return false;
			target = app.m_tourCreatedCameraNodes[k];
		} else {
			target = app.ClusterToWorld(tc.node);
		}
		if (target == UINT32_MAX
			|| target >= app.m_world->scene.hierarchy.size())
			return false;
		if (!app.FindNodeCamera(target))
			return false; // not a camera: orphan, preserved verbatim
		out.verb = EditVerb::SetCamAttr;
		out.node = target;
		out.camAttr = tc.text;
		// Before-state from the live world (exact undo).
		if (!app.ReadNodeCameraAttr(target, tc.text, out.beforeVec4,
				out.beforeNum, out.beforeText))
			return false;
		if (tc.text == "position" || tc.text == "rotation") {
			out.vec4 = tc.vec4;
		} else 		if (tc.text == "name") {
			out.afterText = tc.name;
		} else {
			out.number = tc.number;
		}
		return true;
	}
	case smvk::tour::Verb::SetExportAttr: {
		uint32_t target = UINT32_MAX;
		if ((tc.node & smvk::tour::kCreateExportRefBase) != 0) {
			const uint32_t k = tc.node & ~smvk::tour::kCreateExportRefBase;
			if (k >= app.m_tourCreatedExportNodes.size())
				return false;
			target = app.m_tourCreatedExportNodes[k];
		} else {
			target = app.ClusterToWorld(tc.node);
		}
		if (target == UINT32_MAX
			|| target >= app.m_world->scene.hierarchy.size())
			return false;
		if (!(FindExport(app.m_world->scene, target) != nullptr))
			return false; // not the exportSettings node: orphan
		out.verb = EditVerb::SetExportAttr;
		out.node = target;
		out.exportAttr = tc.text;
		// Before-state from the live world (exact undo).
		if (!app.ReadNodeExportAttr(target, tc.text, out.beforeNum,
				out.beforeFlag, out.beforeText))
			return false;
		if (tc.text == "meshopt" || tc.text == "lods"
			|| tc.text == "lightmap" || tc.text == "cubemap"
			|| tc.text == "cubemapOctree" || tc.text == "meshlets"
			|| tc.text == "giVolumes" || tc.text == "giWorld"
			|| tc.text == "autoKtx" || tc.text == "ignoreVfs") {
			out.afterFlag = tc.flag;
		} else if (tc.text == "lodRatios" || tc.text == "lmExcludeKw") {
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
	// The scene's exportSettings NODE restores the cook knobs — the
	// tour carries it as createNode -at exportSettings + setExportAttr
	// lines (replayed below like any create). outPak/contentDir stay
	// session-only (never persisted).
	// Replay into the stack (fresh import underneath, so before-states are
	// exact and the whole tour stays undoable). Wide rules load AFTER the
	// direct commands so locals win over matching regex (more specific
	// wins); rules skip locally-overridden nodes when they apply.
	// TEXTURE-asset pool imports replay FIRST (before the material
	// records): the records mirror the session's texture pool imports,
	// so .texture pool entries (light cookies, material tex refs) resolve
	// exactly as authored. Failed records stay in the mirror (the next
	// save re-emits them verbatim — the orphan-command contract: a reload
	// must never erase records; restoring the file heals the next replay).
	m_textureAssets = doc.textureAssets; // session mirror (save re-emits)
	// Replay mode FIRST: the import loops below call the shared imports,
	// and their record pushes must stay gated (the records arrive from
	// the doc here — a second push would duplicate every entry).
	m_replayingTour = true;
	for (const auto& ta : doc.textureAssets) {
		// Resolve: project-relative records against the PROJECT ROOT
		// first (the record's in-project convention), then as-is for
		// absolute/out-of-project records.
		std::filesystem::path hp;
		std::error_code ec;
		if (!m_projectRoot.empty())
			hp = std::filesystem::path(m_projectRoot) / ta.path;
		if (hp.empty() || !std::filesystem::exists(hp, ec))
			hp = std::filesystem::path(ta.path);
		smvk::Cluster* cl =
			m_world->clusters.empty() ? nullptr
			: m_world->clusters[0].get();
		if (!cl)
			break;
		std::string imErr;
		const smvk::TextureHandle h =
			smvk::assets::ImportTextureAssetToCluster(
				*cl, hp, m_context.get(), imErr);
		if (h.empty()) {
			Log("tour: textureAsset import failed: '" + ta.path
				+ "': " + (imErr.empty()
					? "not found (under the project root, then as-is)"
					: imErr)
				+ " (cookie/material refs to it resolve empty).");
			continue;
		}
		RebuildTexHandleMap();
	}
	// Material-asset POOL imports replay FIRST: the records mirror the
	// session's pool creations in order, so the pool indices the later
	// assignMaterial/-m commands carry resolve exactly as authored.
	// Failed records stay in the mirror (the next save re-emits them
	// verbatim — the orphan-command contract: a reload must never erase
	// records; restoring the file heals the next replay).
	m_materialAssets = doc.materialAssets; // session mirror (save re-emits)
	// Replay mode FIRST: the materialAsset loop below calls
	// EnsureDefaultMaterial + the shared import, and their record pushes
	// must stay gated (the records arrive from the doc there — a second
	// push would duplicate every entry in the next save).
	for (const auto& ma : doc.materialAssets) {
		if (ma.editorDefault) {
			// The deterministic seed (records no new entry here:
			// m_replayingTour gates EnsureDefaultMaterial's push,
			// and the mirror above already carries the record).
			(void)EnsureDefaultMaterial();
			continue;
		}
		// Resolve: project-relative records against the PROJECT ROOT
		// first (the record's in-project convention — as-is/CWD would hit
		// the bundled content tree over the project for same-named
		// sources), then as-is for absolute/out-of-project records.
		std::filesystem::path hp;
		std::error_code ec;
		if (!m_projectRoot.empty())
			hp = std::filesystem::path(m_projectRoot) / ma.path;
		if (hp.empty() || !std::filesystem::exists(hp, ec))
			hp = std::filesystem::path(ma.path);
		smvk::Cluster* cl =
			m_world->clusters.empty() ? nullptr
			: m_world->clusters[0].get();
		if (!cl)
			break;
		std::string imErr;
		const smvk::ClusterMaterialHandle h =
			smvk::assets::ImportMaterialAssetToCluster(
				*cl, hp, m_context.get(), imErr);
		if (h.empty()) {
			Log("tour: materialAsset import failed: '" + ma.path
				+ "': " + (imErr.empty()
					? "not found (under the project root, then as-is)"
					: imErr)
				+ " — later material indices may shift.");
			continue;
		}
		RebuildTexHandleMap();
		const uint32_t worldIdx =
			uint32_t(m_world->materialLocations.size());
		m_world->materialLocations.push_back({ cl, h });
	}
	m_tourCreatedNodes.clear();
	m_tourCreatedGiVolumeNodes.clear();
	m_tourCreatedCubemapNodes.clear();
	m_tourCreatedCameraNodes.clear();
	m_tourCreatedExportNodes.clear();
	// (m_replayingTour armed above, before the materialAsset replay.)
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
	// GI-asset bindings (the session mirror + the LIVE-SCENE resolution):
	// the mirror round-trips through save (the records are doc state,
	// never derived here — only the bake owner writes them); the
	// per-volume bindings resolve onto the live scene through the SAME
	// family list the setGiVolumeAttr replay used (m_tourCreatedGiVolumeNodes,
	// filled in command order above), so the editor's loaders (the GI-volume
	// preview) read exactly what the scene carries — no stem guessing.
	// Clear-first keeps a re-replay idempotent (stale bindings from the
	// previous pass die).
	m_giAssets = doc.giAssets;
	if (m_world) {
		Scene& liveScene = m_world->scene;
		liveScene.giWorldSource.clear();
		liveScene.giWorldGuid.clear();
		for (GiVolumeRow& _row : liveScene.tables.giVolumes) {
		NodeId node = _row.node; auto& def = _row.volume;
			def.bakeSource.clear();
			def.bakeGuid.clear();
		}
		for (const smvk::tour::GiAssetCmd& ga : m_giAssets) {
			if (ga.source.empty())
				continue; // half-written record: skip (mirrors the writer)
			if (ga.kind == smvk::tour::GiAssetCmd::Kind::World) {
				if (!liveScene.giWorldSource.empty())
					continue; // duplicate world record: first kept
				liveScene.giWorldSource = ga.source;
				liveScene.giWorldGuid = ga.guid;
				continue;
			}
			const uint32_t k =
				ga.ref & ~smvk::tour::kCreateGiVolumeRefBase;
			if ((ga.ref & smvk::tour::kCreateGiVolumeRefBase) == 0
					|| k >= m_tourCreatedGiVolumeNodes.size()) {
				Log("tour: giAsset GI-volume ref out of range (dropped — '"
					+ ga.source + "')");
				continue;
			}
			GiVolumeRow* it = FindGiVolume(liveScene,
				m_tourCreatedGiVolumeNodes[k]);
			if (!it)
				continue;
			it->volume.bakeSource = ga.source;
			it->volume.bakeGuid = ga.guid;
		}
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
	LoadDefaultCameraFromDoc(doc); // world default camera (typed ref field)
	LoadPlacementFromDoc(doc); // scatter graphs (place.* editorSet lines)
	LoadImagesFromDoc(doc); // image definitions (img.* editorSet lines)
	// (No path-remap editorSettings anymore: texture relinks are input
	// plugs on the source assets' .meta sidecars, applied at import.)
	// References were already bound at import (LoadGltfIntoWorld runs
	// ApplySceneReferences on this same file); mirror the list so the
	// editor state matches the parsed doc exactly.
	if (!doc.references.empty())
		m_references = doc.references;
	// Auto-seed the exportSettings singleton: every open scene carries
	// exactly one (WorldSettings scaffold). Idempotent — a doc that
	// already created it (createNode -at exportSettings) is untouched.
	EnsureExportSettingsNode();
	// Re-resolve the convex-volume preview AFTER the replay: the
	// giAsset bindings ride the replayed GI-volume definitions (resolved
	// above through the family list), so the pre-replay call inside the
	// scene-load path always saw an empty scene. The reload keeps the
	// preview honest after every doc replay (undo restores, too).
	TryLoadGiVolumes();
	// The replay rebuilt the world in place (same object).
	return true;
}

// ---------------------------------------------------------------------------
// Scene image definitions (Maya file nodes): images live OUTSIDE the DAG
// like materials — authoring references (path + tiling/colorspace/filter)
// persisted as img.* editorSettings (legacy) AND cooked fileNode records
// (the cooker reads them for KTX conversion; fileNodes win on load). A
// material using one mints the cluster texture resource at use time.
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
	// Cooked twin: image defs ARE file nodes — the cooker reads these
	// for KTX conversion (path + colorspace + sampler intent). The img.*
	// settings above stay for older builds; fileNodes win on load.
	for (const ImageDef& img : m_images) {
		if (img.path.empty())
			continue;
		smvk::tour::FileNode fn;
		fn.name = img.name;
		fn.path = img.path;
		fn.colorspace = img.colorspace;
		fn.wrap = img.wrap;
		fn.filter = img.filter;
		doc.fileNodes.push_back(std::move(fn));
	}
}

void EditorApp::LoadImagesFromDoc(const smvk::tour::Doc& doc) {
	// Cooked file nodes win: current files persist image defs as
	// fileNode records (read by the cooker); legacy img.* settings cover
	// older tours.
	if (!doc.fileNodes.empty()) {
		m_images.clear();
		uint64_t id = 1;
		for (const auto& fn : doc.fileNodes) {
			if (fn.path.empty())
				continue;
			ImageDef def;
			def.id = id++;
			def.name = fn.name.empty()
				? std::filesystem::path(fn.path).stem().string() : fn.name;
			def.path = fn.path;
			def.wrap = std::clamp(fn.wrap, 0, 2);
			def.colorspace = std::clamp(fn.colorspace, 0, 1);
			def.filter = std::clamp(fn.filter, 0, 1);
			m_images.push_back(std::move(def));
		}
		m_nextImageId = id;
		return;
	}
	auto u64 = [](const std::string& s, uint64_t fb) {
		// (One explicit return type: std::stoull's unsigned long long and
		// the uint64_t fb are distinct types on LP64 - MSVC folds them,
		// GCC/Clang reject the inconsistent deduction.)
		try { return s.empty() ? fb : static_cast<uint64_t>(std::stoull(s)); }
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
			// Source scenes only (paks are refused — they run in the App).
			if (HasPakExtension(src)) {
				Log("tour open: source '" + src
					+ "' is a cooked pak — open the source scene instead.");
				return false;
			}
			if (!LoadGltfIntoWorld(src)) {
				Log("tour open: failed to load source '" + src + "'");
				return false;
			}
		} else if (!m_world) {
			Log("tour open: no scene open and no loadable source.");
			return false;
		}
	} else {
		// Definition tour (the scene's glTF rides as root reference
		// #0): ALWAYS rebuild the world from the document — the bind
		// and the replay below need the fresh deterministic ids, never
		// whatever scene happens to be open.
		if (!LoadGltfIntoWorld({}, path)) {
			Log("tour open: failed to build the scene from the definition.");
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
		const MeshRow* meshRow = FindMesh(scene, tileNode);
		if (!meshRow)
			continue;
		const uint32_t worldMeshIdx = meshRow->mesh;
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
	SetNodeNameIndex(scene, uint32_t(group), nameId);
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
	// Tile split clamp: every tile needs >= one sample interval per axis
	// (BuildTerrainTileUpload rejects finer splits — a one-sample tile
	// cannot tile UVs). Clamping HERE (not just rejecting the mesh) keeps
	// the group creation atomic: every tile node gets its mesh. The tour
	// replay clamps identically, so editor and cook agree by construction.
	const uint32_t maxTiles = std::min(smvk::placement::kTerrainMaxTiles,
		g.data.params.res - 1u);
	g.data.tilesX = std::clamp(uint32_t(std::max(c.node2 & 0xFFFFu, 1u)),
		1u, maxTiles);
	g.data.tilesZ = std::clamp(
		uint32_t(std::max((c.node2 >> 16) & 0xFFFFu, 1u)),
		1u, maxTiles);
	g.tileNodes.resize(size_t(g.data.tilesX) * g.data.tilesZ);
	const uint32_t material = (c.material != UINT32_MAX
		&& c.material < m_world->materialLocations.size())
		? c.material : EnsureDefaultMaterial();
	smvk::Cluster* cl = m_world->clusters[0].get();
	// Tile children: node + mesh + world wiring + draw entry each.
	// Tile position = its ACTUAL first sample (TerrainTileOrigin — the
	// integer sample split does not align with the even metric split
	// unless tiles divide the interval count; see the seam contract in
	// placement_terrain.h).
	for (uint32_t tz = 0; tz < g.data.tilesZ; ++tz)
		for (uint32_t tx = 0; tx < g.data.tilesX; ++tx) {
			const int tileId = addNode(scene, group, level + 1);
			if (tileId < 0)
				return;
			const uint32_t t = uint32_t(tileId);
			const glm::vec2 tileOrigin =
				smvk::placement::TerrainTileOrigin(g.data, tx, tz);
			scene.localTransform[size_t(t)] = glm::translate(
				glm::mat4(1.0f),
				glm::vec3(tileOrigin.x, 0.0f, tileOrigin.y));
			const uint32_t tName = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.afterText + "_" + std::to_string(tx)
				+ "_" + std::to_string(tz));
			SetNodeNameIndex(scene, t, tName);
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
			SetMesh(scene, t, worldMeshIdx);
			SetMaterial(scene, t, material);
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
		RemoveMesh(scene, t);
		RemoveMaterial(scene, t);
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
	// KTX2 payload passes through; other image sources (PNG/JPG/...)
	// transcode to KTX2 (the importer's own conversion) so the Path
	// editor + material slots can relink against the ORIGINAL source
	// files too — a bare PNG used to fail the KTX load with no way to
	// fix a broken texture link. HDR needs a float transcode (skip).
	std::vector<uint8_t> payload = bytes; // what the GPU gets
	const bool isKtx2 = bytes.size() >= 8
		&& bytes[0] == 0xAB && bytes[1] == 'K' && bytes[2] == 'T'
		&& bytes[3] == 'X' && bytes[4] == ' ' && bytes[5] == '2'
		&& bytes[6] == '0' && bytes[7] == 0xBB;
	if (!isKtx2) {
		std::string ext =
			std::filesystem::path(path).extension().string();
		for (char& c : ext)
			c = char(std::tolower((unsigned char)c));
		if (ext == ".hdr" || ext == ".exr") {
			Log("Material editor: HDR sources need a float transcode "
				"(unsupported here): " + path);
			return -1;
		}
		std::string terr;
		std::vector<uint8_t> ktx;
		// sRGB transfer (color textures): normal maps want linear —
		// prefer a pre-converted .ktx2 for those.
		if (!TranscodeImageToKtx2(bytes.data(), bytes.size(), true,
				path, ktx, terr)) {
			Log("Material editor: texture transcode failed '" + path
				+ "': " + terr);
			return -1;
		}
		payload = std::move(ktx);
		Log("Material editor: transcoded '" + path + "' to KTX2 "
			"(sRGB).");
	}
	auto holder = smvk::LoadKtxTextureMemory(*m_context, payload.data(),
		payload.size(), path);
	if (!holder.valid()) {
		Log("Material editor: KTX2 load failed: " + path);
		return -1;
	}
	const smvk::TextureHandle h = holder;
	cl->textures.create(std::move(holder));
	cl->RegisterTextureUri(path, h);
	// The ORIGINAL source bytes ride the pool entry (cooks re-embed +
	// re-convert from source, exactly like imported images).
	cl->SetTextureSource(path, std::move(bytes));
	// Refresh pool->RHI-handle + bindless maps so the renderer can bind
	// the new texture this frame.
	RebuildTexHandleMap();
	Log("Material editor: added texture '" + path + "' (pool "
		+ std::to_string(h.index()) + ").");
	return int(h.index());
}

std::string EditorApp::DerivedAssetGuid(const std::string& assetPath) {
	// A derived asset's GUID lives in the sibling glTF manifest (the
	// derivation wrote it). Scan the directory's .gltf.meta files for
	// the entry whose file matches.
	if (assetPath.empty() || !m_world)
		return {};
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path dir = fs::path(assetPath).parent_path();
	const std::string want =
		fs::path(assetPath).filename().generic_string();
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
			it.increment(ec)) {
		if (ec || !it->is_regular_file(ec))
			continue;
		const std::string n = it->path().filename().string();
		if (n.size() <= 10
			|| n.compare(n.size() - 10, 10, ".gltf.meta") != 0)
			continue;
		smvk::assets::AssetManifest man;
		std::string merr;
		if (!smvk::assets::LoadAssetManifest(it->path().string(), man,
				merr))
			continue;
		for (const auto& e : man.entries)
			if (e.file == want)
				return e.guid;
	}
	return {};
}

void EditorApp::ImportCookieTexture(const std::string& cookieRef) {
	// Spot-cookie LIVE import (see the header): the cookie ref's
	// .texture lands in cluster 0's pool NOW (the live view's light
	// build resolves it through FindTextureByUri) + a textureAsset
	// record rides the tour (the cook replays it before the command
	// list). Empty clears the slot (no import); non-.texture refs are
	// refused with a log (a raw image is not cluster-registered —
	// import it as a .texture first). NOT during replay (the tour's
	// records replay there). The record dedupes by path (one .texture
	// cooks cookies on any number of lights).
	if (cookieRef.empty() || m_replayingTour || !m_world
		|| m_world->clusters.empty() || !m_world->clusters[0]
		|| !m_context)
		return;
	{
		std::string ext =
			std::filesystem::path(cookieRef).extension().string();
		for (char& c : ext)
			c = char(std::tolower((unsigned char)c));
		if (ext != ".texture") {
			Log("Cookie: '" + cookieRef + "' is not a .texture asset — "
				"reference a .texture (Browse to import an image, or "
				"select an image file + 'Import as .texture').");
			return;
		}
	}
	// Resolve: project-relative refs against the PROJECT ROOT first
	// (the record's in-project convention), then as-is.
	std::filesystem::path hp;
	{
		std::error_code ec;
		if (!m_projectRoot.empty())
			hp = std::filesystem::path(m_projectRoot) / cookieRef;
		if (hp.empty() || !std::filesystem::exists(hp, ec))
			hp = std::filesystem::path(cookieRef);
	}
	smvk::Cluster* cl = m_world->clusters[0].get();
	std::string imErr;
	const smvk::TextureHandle h =
		smvk::assets::ImportTextureAssetToCluster(
			*cl, hp, m_context.get(), imErr);
	if (h.empty()) {
		Log("Cookie: texture import failed for '" + cookieRef + "': "
			+ (imErr.empty() ? "not found" : imErr)
			+ " (the light renders unshadowed until it resolves).");
		return;
	}
	RebuildTexHandleMap();
	// Record the pool import (the tour's textureAsset list — deduped by
	// path so one .texture cooks every light's cookie).
	{
		std::string posix = cookieRef;
		for (char& c : posix)
			if (c == '\\')
				c = '/';
		bool known = false;
		for (const auto& ta : m_textureAssets)
			if (ta.path == posix) {
				known = true;
				break;
			}
		if (!known) {
			smvk::tour::TextureAssetCmd rec{};
			rec.path = posix;
			rec.name = std::filesystem::path(cookieRef).stem().string();
			m_textureAssets.push_back(std::move(rec));
		}
	}
	Log("Cookie: imported '" + cookieRef + "' (live view + the tour's "
		"textureAsset record).");
}

void EditorApp::AssignMaterialAsset(const std::string& assetPath) {
	// MATERIAL ASSET drop (browser -> viewport): load the .material
	// file, create the cluster material (values + textures from its
	// refs — the image files beside the source glTF), assign it to the
	// SELECTED subtree (one undoable AssignMaterial).
	if (!m_world || m_world->clusters.empty()
		|| !m_world->clusters[0] || !m_context)
		return;
	if (m_selected == UINT32_MAX
		|| m_selected >= m_world->scene.hierarchy.size()) {
		Log("Assets: material drop needs a selection (select a node "
			"first).");
		return;
	}
	smvk::assets::MaterialAsset m;
	std::string err;
	if (!smvk::assets::LoadMaterialAsset(assetPath, m, err)) {
		Log("Assets: material load failed: " + err);
		return;
	}
	smvk::Cluster* cl = m_world->clusters[0].get();
	// SHARED import (the cook replays the same path from the tour's
	// materialAsset records — pool determinism across the session and
	// the cook replay; see ImportMaterialAssetToCluster).
	smvk::ClusterMaterialHandle imported{};
	{
		std::string imErr;
		imported = smvk::assets::ImportMaterialAssetToCluster(
			*cl, assetPath, m_context.get(), imErr);
		if (imported.empty()) {
			Log("Assets: material creation failed for '" + assetPath
				+ "': " + imErr);
			return;
		}
		// Pool->RHI-handle refresh (editor-side maps; the shared import
		// touched the cluster pools).
		RebuildTexHandleMap();
		// Record the pool import (the tour's materialAsset list — in
		// creation order so replay reproduces the pool order). NOT during
		// tour replay: the records arrive from the doc there.
		if (!m_replayingTour) {
			smvk::tour::MaterialAssetCmd rec{};
			const std::string rel = ProjectRelativePath(assetPath);
			std::string posix = rel.empty() ? assetPath : rel;
			for (char& c : posix)
				if (c == '\\')
					c = '/';
			rec.path = posix; // project-relative when in-project (the
			                  // reference-source convention), else absolute
			rec.name = m.name;
			m_materialAssets.push_back(std::move(rec));
		}
	}
	const uint32_t worldIdx =
		uint32_t(m_world->materialLocations.size());
	m_world->materialLocations.push_back({ cl, imported });
	// Undoable assign on the selection.
	EditCmd c{};
	c.verb = EditVerb::AssignMaterial;
	c.node = m_selected;
	c.material = worldIdx;
	uint32_t before = UINT32_MAX;
	if (const MaterialRow* row = FindMaterial(m_world->scene, m_selected))
		before = row->material;
	if (before == UINT32_MAX) {
		// No material on the node: seed a default first so the assign
		// round-trips (undo restores the default).
		const uint32_t def = EnsureDefaultMaterial();
		if (def != UINT32_MAX) {
			SetMaterial(m_world->scene, m_selected, def);
			before = def;
		}
	}
	c.beforeMaterial = before;
	Do(c);
	Log("Assets: assigned material '" + m.name + "' (" + m.guid
		+ ") to node " + std::to_string(m_selected) + ".");
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
	// (e.g. a pak scene cooked from a tour that minted one) — NO pool
	// growth, so no tour record (records mirror pool CREATIONS only).
	if (auto hit = cl->FindMaterialByUri("editor://default")) {
		const uint32_t worldIdx = uint32_t(m_world->materialLocations.size());
		m_world->materialLocations.push_back({ cl, *hit });
		m_defaultMatWorld = worldIdx;
		return worldIdx;
	}
	// SHARED seed creation (the deterministic material; cooks replay it
	// from the tour's materialAsset records — EnsureClusterDefaultMaterial).
	const smvk::ClusterMaterialHandle h =
		smvk::assets::EnsureClusterDefaultMaterial(*cl);
	if (h.empty())
		return UINT32_MAX;
	// Pool creation: record it (ordered — the record order IS the pool
	// order; a later materialAsset import lands after this one). NOT
	// during tour replay: the record arrives from the doc there.
	if (!m_replayingTour)
		m_materialAssets.push_back(smvk::tour::MaterialAssetCmd{
			/*editorDefault=*/true, /*path=*/{}, /*name=*/"default" });
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

