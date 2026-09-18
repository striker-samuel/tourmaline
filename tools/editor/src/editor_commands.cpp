// Editor CRUD command stack: execute/do/undo/redo + .tour persistence.
//
// Live commands use WORLD-space ids; the .tour file uses CLUSTER-space ids
// (import-deterministic). Translation happens only at the save/load
// boundary (CmdToTour / TourToCmd).

#include "editor_app.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <Engine/Asset/tour.h>
#include <Engine/Flintbase/flintbase_backend.h>
#include <Engine/Vfs/vfs.h>

namespace editor {

void EditorApp::ReadLocalTRS(uint32_t node, glm::vec3& t,
	glm::vec3& rDeg, glm::vec3& s) {
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

void EditorApp::ExecCmd(const EditCmd& c, bool forward) {
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
	case EditVerb::Parent: {
		ReparentNode(c.node, forward ? c.node2 : c.beforeParent);
		break;
	}
	}
}

void EditorApp::Do(const EditCmd& c) {
	ExecCmd(c, true);
	m_history.erase(m_history.begin() + m_historyCursor, m_history.end());
	m_history.push_back(c);
	m_historyCursor = m_history.size();
	while (m_history.size() > kHistoryCap) {
		m_history.erase(m_history.begin());
		--m_historyCursor;
	}
	SaveTour();
}

void EditorApp::PushDone(const EditCmd& c) {
	// Already executed live (drag path): just record + save.
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
	SaveTour();
	return true;
}

bool EditorApp::Redo() {
	if (m_historyCursor >= m_history.size())
		return false;
	ExecCmd(m_history[m_historyCursor], true);
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
	case editor::EditVerb::Parent: return "reparent";
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
	case EditVerb::Rename:
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::Rename;
		out.text = c.afterText;
		return true;
	case EditVerb::Translate:
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::SetTranslate;
		out.vec = c.afterVec;
		return true;
	case EditVerb::Rotate:
		if (!needNode())
			return false;
		out.verb = smvk::tour::Verb::SetRotate;
		out.vec = c.afterVec;
		return true;
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
	doc.hasCook = true;
	doc.cook.outPak = m_cook.outPak;
	doc.cook.meshopt = m_cook.meshopt;
	doc.cook.lods = m_cook.lods;
	doc.cook.lightmap = m_cook.lightmap;
	doc.cook.lmDensity = m_cook.lmDensity;
	doc.cook.lmLobes = m_cook.lmLobes;
	doc.cook.lmAtlasWidth = m_cook.lmAtlasWidth;
	doc.cook.lmBudget = m_cook.lmBudget;
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

// Build a live EditCmd from a tour command (cluster->world), capturing the
// exact before-state from the live world for undo.
bool EditorApp::TourToCmd(EditorApp& app, const smvk::tour::Command& tc, EditCmd& out) {
	out = EditCmd{};
	const uint32_t wn = app.ClusterToWorld(tc.node);
	auto needNode = [&]() {
		if (wn == UINT32_MAX || wn >= app.m_world->scene.hierarchy.size())
			return false;
		out.node = wn;
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
	if (!doc.fingerprint.empty()) {
		const std::string now = smvk::tour::Fingerprint(m_gltfPath);
		if (!now.empty() && now != doc.fingerprint)
			Log("tour: source changed since save (ids may have shifted)");
	}
	// Cook block restores the panel.
	if (doc.hasCook) {
		m_cook.outPak = doc.cook.outPak;
		m_cook.meshopt = doc.cook.meshopt;
		m_cook.lods = doc.cook.lods;
		m_cook.lightmap = doc.cook.lightmap;
		m_cook.lmDensity = doc.cook.lmDensity;
		m_cook.lmLobes = doc.cook.lmLobes;
		m_cook.lmAtlasWidth = doc.cook.lmAtlasWidth;
		m_cook.lmBudget = doc.cook.lmBudget;
		m_cook.lmRays = doc.cook.lmRays;
		m_cook.lmExcludeSmall = doc.cook.lmExcludeSmall;
		m_cook.lmExcludeKw = doc.cook.lmExcludeKw;
		m_cook.sunAzimuth = doc.cook.sunAzimuth;
		m_cook.sunElevation = doc.cook.sunElevation;
		m_cook.sunIntensity = doc.cook.sunIntensity;
		m_cook.skyIntensity = doc.cook.skyIntensity;
	}
	// Replay into the stack (fresh import underneath, so before-states are
	// exact and the whole tour stays undoable).
	ClearHistory();
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
	m_historyCursor = m_history.size();
	Log("tour: applied " + std::to_string(applied) + "/" +
		std::to_string(doc.commands.size()) + " commands.");
	return true;
}

} // namespace editor
