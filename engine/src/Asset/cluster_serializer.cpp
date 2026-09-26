#include <Engine/Asset/cluster_serializer.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/asset_registry.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Utils/texture_loader.h>

namespace smvk {
namespace {

// Little-endian binary helpers (pak format is LE; hosts are LE — see pak.cpp).
void PutU32(std::vector<uint8_t>& out, uint32_t v) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + 4);
}
void PutI32(std::vector<uint8_t>& out, int32_t v) {
	PutU32(out, static_cast<uint32_t>(v));
}
void PutU64(std::vector<uint8_t>& out, uint64_t v) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + 8);
}
void PutF32(std::vector<uint8_t>& out, float v) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + 4);
}
void PutStr(std::vector<uint8_t>& out, StringViewT s) {
	PutU32(out, static_cast<uint32_t>(s.size()));
	out.insert(out.end(), s.begin(), s.end());
}
void PutVec3(std::vector<uint8_t>& out, const glm::vec3& v) {
	PutF32(out, v.x);
	PutF32(out, v.y);
	PutF32(out, v.z);
}

struct Cursor {
	std::span<const uint8_t> img;
	size_t off = 0;
	bool ok = true;
	uint32_t u32() {
		uint32_t v = 0;
		if (off + 4 > img.size())
			ok = false;
		else {
			memcpy(&v, img.data() + off, 4);
			off += 4;
		}
		return v;
	}
	int32_t i32() { return static_cast<int32_t>(u32()); }
	uint64_t u64() {
		uint64_t v = 0;
		if (off + 8 > img.size())
			ok = false;
		else {
			memcpy(&v, img.data() + off, 8);
			off += 8;
		}
		return v;
	}
	float f32() {
		float v = 0.0f;
		if (off + 4 > img.size())
			ok = false;
		else {
			memcpy(&v, img.data() + off, 4);
			off += 4;
		}
		return v;
	}
	std::string str() {
		const uint32_t n = u32();
		std::string s;
		if (!ok)
			return s;
		if (n > 16 * 1024 * 1024 || off + n > img.size()) {
			ok = false;
			return s;
		}
		s.assign(reinterpret_cast<const char*>(img.data() + off), n);
		off += n;
		return s;
	}
	glm::vec3 vec3() {
		const float x = f32(), y = f32(), z = f32();
		return glm::vec3(x, y, z);
	}
};

float SphereRadiusForAabb(const glm::vec3& mn, const glm::vec3& mx) {
	return glm::length(mx - mn) * 0.5f;
}

// Scene blob TOC path: index 0 keeps the historic path so single-scene
// paks are byte-stable; further scenes append their index.
std::string SceneTocPath(size_t index, bool multi) {
	if (!multi || index == 0)
		return "<cluster-local>#scene";
	return "<cluster-local>#scene." + std::to_string(index);
}

// Index encoded in a scene TOC path ("#scene" -> 0, "#scene.N" -> N;
// anything else sorts last and fails loudly on load, never silently).
uint32_t SceneBlobIndex(const std::string& tocPath) {
	const size_t hash = tocPath.find_last_of('#');
	const std::string leaf = (hash == std::string::npos)
		? tocPath : tocPath.substr(hash + 1);
	if (leaf == "scene")
		return 0;
	if (leaf.rfind("scene.", 0) == 0) {
		uint32_t n = 0;
		bool digits = false;
		for (size_t i = 6; i < leaf.size(); ++i) {
			if (leaf[i] < '0' || leaf[i] > '9')
				return 0xFFFFFFFFu;
			digits = true;
			n = n * 10 + uint32_t(leaf[i] - '0');
		}
		return digits ? n : 0xFFFFFFFFu;
	}
	return 0xFFFFFFFFu;
}

} // namespace

// ---------------------------------------------------------------------------
// Serialize
// ---------------------------------------------------------------------------

bool SerializeClusters(
	const std::vector<const Cluster*>& clusters,
	const std::vector<std::string>& sceneNames,
	PakWriter& writer,
	ClusterSerializeStats* stats,
	std::string* err) {
	auto fail = [&](const std::string& msg) {
		if (err)
			*err = msg;
		std::cerr << "SerializeCluster: " << msg << std::endl;
		return false;
	};
	if (clusters.empty())
		return fail("no clusters to serialize");
	for (size_t ci = 0; ci < clusters.size(); ++ci)
		if (!clusters[ci])
			return fail("null cluster #" + std::to_string(ci));
	const bool multi = clusters.size() > 1;

	// Shared blob pool: asset URIs are stable across per-scene clusters
	// of one model, so each unique blob is written once and every scene
	// references the shared pak ids.
	std::unordered_map<std::string, uint32_t> meshIdByUri, matIdByUri,
		lightIdByUri, camIdByUri;
	std::unordered_set<std::string> texturesDone;
	uint64_t textureBytes = 0;
	struct LocalMaps {
		std::unordered_map<uint32_t, uint32_t> mesh, mat, light, camera;
	};
	std::vector<LocalMaps> locals(clusters.size());

	for (size_t ci = 0; ci < clusters.size(); ++ci) {
	const Cluster& cluster = *clusters[ci];
	LocalMaps& local = locals[ci];

	// --- Meshes (pool index order via forEachHandle) ---------------------
	std::vector<ClusterStaticMeshHandle> meshHandles;
	{
		// const_cast: Pool::forEachHandle is non-const (historic). It does
		// not mutate when the functor only reads.
		const_cast<Cluster&>(cluster).staticMeshes.forEachHandle(
			[&](ClusterStaticMeshHandle h) { meshHandles.push_back(h); });
	}
	std::vector<std::string> meshUris;
	meshUris.reserve(meshHandles.size());
	for (uint32_t k = 0; k < meshHandles.size(); ++k) {
		const auto h = meshHandles[k];
		std::string uri = cluster.GetMeshUri(h).Str();
		if (uri.empty())
			uri = "<cluster-local>#mesh." + std::to_string(h.index());
		meshUris.push_back(uri);
		auto idIt = meshIdByUri.find(uri);
		if (idIt != meshIdByUri.end()) {
			local.mesh[h.index()] = idIt->second; // shared with a sibling scene
			continue;
		}
		const uint32_t pakId = static_cast<uint32_t>(meshIdByUri.size());
		meshIdByUri[uri] = pakId;
		local.mesh[h.index()] = pakId;

		const StaticMesh* mesh = const_cast<Cluster&>(cluster).staticMeshes.get(h);
		if (!mesh)
			return fail("stale mesh handle in pool");
		// Full vertex/index payloads only exist when the cluster was
		// imported with retained uploads (cooker path). Runtime clusters
		// hold GPU-side allocations only and cannot be serialized.
		const StaticMeshUpload* upload = cluster.GetRetainedUpload(h);
		if (!upload)
			return fail("no retained upload for mesh: " + uri +
				" (import with Cluster::SetRetainUploads(true) to cook paks)");

		std::vector<uint8_t> blob;
		PutStr(blob, uri);
		const Geometry& g = mesh->geometry;
		// NOTE: the GPU-side allocation (VMA offsets, firstIndex/baseVertex)
		// is session-local and never stored. The source vertex/index arrays,
		// AABB/sphere and topology are persisted; the upload path rebuilds
		// GPU surfaces on load.
		PutU32(blob, static_cast<uint32_t>(upload->vertices.size()));
		PutU32(blob, static_cast<uint32_t>(upload->indices.size()));
		PutU32(blob, static_cast<uint32_t>(upload->surfaces.size()));
		for (const Surface& s : upload->surfaces) {
			PutU32(blob, s.firstIndex);
			PutU32(blob, s.indexCount);
			PutI32(blob, s.baseVertex);
		}
		PutVec3(blob, upload->aabbMin);
		PutVec3(blob, upload->aabbMax);
		const float sphere = g.sphereRadius > 0.0f
			? g.sphereRadius
			: SphereRadiusForAabb(upload->aabbMin, upload->aabbMax);
		PutF32(blob, sphere);
		PutU32(blob, static_cast<uint32_t>(g.topology));
		// LOD marker (instrumentation only; renderer ignores it).
		LodInfo lod;
		if (auto hit = cluster.GetMeshLod(uri))
			lod = *hit;
		else {
			lod.groupId = h.index();
			lod.triCount = static_cast<uint32_t>(upload->indices.size() / 3);
		}
		PutU32(blob, lod.groupId);
		PutU32(blob, lod.lodIndex);
		PutU32(blob, lod.candidateCount);
		PutF32(blob, lod.simplifyError);
		PutF32(blob, lod.screenThreshold);
		PutU32(blob, lod.triCount);
		PutF32(blob, lod.acmr);
		// Lightmap chunk (cooked --lightmap; texCoord2 in the vertex
		// payload below carries the per-vertex CHART-LOCAL uvs + chartId,
		// and the per-chart atlas transforms follow here for the shader
		// unpack: atlasUv = chartUv * xform.xy + xform.zw).
		PutU32(blob, mesh->lmChunk);
		PutU32(blob, static_cast<uint32_t>(mesh->lmChartXforms.size()));
		for (const glm::vec4& xf : mesh->lmChartXforms) {
			PutF32(blob, xf.x);
			PutF32(blob, xf.y);
			PutF32(blob, xf.z);
			PutF32(blob, xf.w);
		}
		// Vertex/index payloads (AoS StaticVertex, u32 indices).
		static_assert(sizeof(StaticVertex) == 76, "StaticVertex layout changed");
		blob.insert(blob.end(),
			reinterpret_cast<const uint8_t*>(upload->vertices.data()),
			reinterpret_cast<const uint8_t*>(upload->vertices.data()) +
				upload->vertices.size() * sizeof(StaticVertex));
		blob.insert(blob.end(),
			reinterpret_cast<const uint8_t*>(upload->indices.data()),
			reinterpret_cast<const uint8_t*>(upload->indices.data()) +
				upload->indices.size() * sizeof(uint32_t));
		// Trailing meshlet section (v2; v1 blobs end here). Always written
		// (possibly count 0) so the layout is uniform for new paks.
		{
			CookedMeshlets cooked;
			if (auto hit = cluster.GetMeshMeshlets(uri))
				cooked = *hit;
			PutU32(blob, static_cast<uint32_t>(cooked.meshlets.size()));
			for (const Meshlet& m : cooked.meshlets) {
				PutU32(blob, m.vertexOffset);
				PutU32(blob, m.vertexCount);
				PutU32(blob, m.triangleOffset);
				PutU32(blob, m.triangleCount);
				PutVec3(blob, m.boundCenter);
				PutF32(blob, m.boundRadius);
				PutVec3(blob, m.coneApex);
				PutVec3(blob, m.coneAxis);
				PutF32(blob, m.coneCutoff);
			}
			PutU32(blob, static_cast<uint32_t>(cooked.vertices.size()));
			for (uint32_t v : cooked.vertices)
				PutU32(blob, v);
			PutU32(blob, static_cast<uint32_t>(cooked.triangles.size()));
			blob.insert(blob.end(), cooked.triangles.begin(), cooked.triangles.end());
			if (stats && !cooked.meshlets.empty()) {
				stats->meshesWithMeshlets++;
				stats->meshletCount += static_cast<uint32_t>(cooked.meshlets.size());
			}
		}

		if (!writer.AddBlob(PakAssetKind::Mesh, uri, std::move(blob)))
			return fail("invalid mesh uri: " + uri);
	}

	// --- Materials ---------------------------------------------------------
	std::vector<ClusterMaterialHandle> matHandles;
	const_cast<Cluster&>(cluster).materials.forEachHandle(
		[&](ClusterMaterialHandle h) { matHandles.push_back(h); });
	std::vector<std::string> matUris;
	matUris.reserve(matHandles.size());

	// RHI texture index -> asset uri (materials store session-local RHI
	// indices; paks store VFS paths).
	std::unordered_map<uint32_t, std::string> rhiIndexToUri;
	for (const auto& [uri, rhi] : cluster.ListTextureUris())
		rhiIndexToUri[rhi.index()] = uri.Str();

	auto texUriForRhi = [&](int rhiIndex) -> std::string {
		if (rhiIndex < 0)
			return {};
		const auto it = rhiIndexToUri.find(static_cast<uint32_t>(rhiIndex));
		return it != rhiIndexToUri.end() ? it->second : std::string{};
	};

	for (uint32_t k = 0; k < matHandles.size(); ++k) {
		const auto h = matHandles[k];
		std::string uri = cluster.GetMaterialUri(h).Str();
		if (uri.empty())
			uri = "<cluster-local>#mat." + std::to_string(h.index());
		matUris.push_back(uri);
		auto idIt = matIdByUri.find(uri);
		if (idIt != matIdByUri.end()) {
			local.mat[h.index()] = idIt->second;
			continue;
		}
		const uint32_t pakId = static_cast<uint32_t>(matIdByUri.size());
		matIdByUri[uri] = pakId;
		local.mat[h.index()] = pakId;

		const Material* mat = const_cast<Cluster&>(cluster).materials.get(h);
		if (!mat)
			return fail("stale material handle in pool");

		std::vector<uint8_t> blob;
		PutStr(blob, uri);
		static_assert(sizeof(MaterialValues) == 128, "MaterialValues layout changed");
		const uint8_t* mv = reinterpret_cast<const uint8_t*>(&mat->values);
		blob.insert(blob.end(), mv, mv + sizeof(MaterialValues));
		PutU32(blob, static_cast<uint32_t>(mat->shadingModel));
		PutU32(blob, static_cast<uint32_t>(mat->alphaMode));
		PutF32(blob, mat->alphaCutoff);
		PutU32(blob, mat->doubleSided ? 1u : 0u);
		PutStr(blob, texUriForRhi(mat->values.textures[0])); // baseColor
		PutStr(blob, texUriForRhi(mat->values.textures[1])); // metallicRoughness
		PutStr(blob, texUriForRhi(mat->values.textures[2])); // normal
		PutStr(blob, texUriForRhi(mat->values.textures[3])); // occlusion
		PutStr(blob, texUriForRhi(mat->values.textures2[0])); // emissive
		// Trailing field (appended after the texture URIs so older
		// readers stop cleanly before it; pre-flag paks read as casting).
		PutU32(blob, mat->castsShadows ? 1u : 0u);

		if (!writer.AddBlob(PakAssetKind::Material, uri, std::move(blob)))
			return fail("invalid material uri: " + uri);
	}

	// --- Textures (embedded .ktx2) ------------------------------------------
	for (const auto& [uri, rhi] : cluster.ListTextureUris()) {
		(void)rhi;
		if (!texturesDone.insert(uri.Str()).second)
			continue; // shared with a sibling scene
		const std::vector<uint8_t>* src = cluster.GetTextureSource(uri);
		if (!src || src->empty())
			return fail("missing embedded .ktx2 source for texture: " + uri.Str() +
				" (importer must stash source bytes via SetTextureSource)");
		TextureSamplerDesc samp;
		if (auto hit = cluster.GetTextureSampler(uri))
			samp = *hit;
		std::vector<uint8_t> blob = BuildTexturePayload(
			uri.Str(), samp.magFilter, samp.minFilter, samp.wrapS, samp.wrapT,
			std::span<const uint8_t>(*src));
		textureBytes += src->size();
		// TextureBlob TOC path = the file part (VFS path) so PakBackend can
		// serve the .ktx2 bytes through the VFS overlay.
		const std::string filePart = AssetFilePart(uri);
		if (filePart.empty())
			return fail("texture uri has no VFS file part: " + uri.Str());
		if (!writer.AddBlob(PakAssetKind::Texture, filePart, std::move(blob)))
			return fail("invalid texture path: " + filePart);
	}

	// --- Lights (one blob per cooked light, URI-keyed like meshes) --------
	std::vector<ClusterLightHandle> lightHandles;
	const_cast<Cluster&>(cluster).lights.forEachHandle(
		[&](ClusterLightHandle h) { lightHandles.push_back(h); });
	for (uint32_t k = 0; k < lightHandles.size(); ++k) {
		const auto h = lightHandles[k];
		std::string uri = cluster.GetLightUri(h).Str();
		if (uri.empty())
			uri = "<cluster-local>#light." + std::to_string(h.index());
		auto idIt = lightIdByUri.find(uri);
		if (idIt != lightIdByUri.end()) {
			local.light[h.index()] = idIt->second;
			continue;
		}
		const uint32_t pakId = static_cast<uint32_t>(lightIdByUri.size());
		lightIdByUri[uri] = pakId;
		local.light[h.index()] = pakId;

		const SceneLight* l = const_cast<Cluster&>(cluster).lights.get(h);
		if (!l)
			return fail("stale light handle in pool");

		std::vector<uint8_t> blob;
		PutStr(blob, uri);
		PutU32(blob, static_cast<uint32_t>(l->type));
		PutVec3(blob, l->color);
		PutF32(blob, l->intensity);
		PutF32(blob, l->range);
		PutF32(blob, l->innerConeAngle);
		PutF32(blob, l->outerConeAngle);
		PutStr(blob, l->name);
		// Trailing flags section (v5 light blobs; v4 blobs end after the
		// name and readers default to 0 — see the read side).
		PutU32(blob, l->flags);

		if (!writer.AddBlob(PakAssetKind::Light, uri, std::move(blob)))
			return fail("invalid light uri: " + uri);
	}

	// --- Cameras (one blob per authored camera, URI-keyed like lights) --
	std::vector<ClusterCameraHandle> cameraHandles;
	{
		// const_cast: Pool::forEachHandle is non-const (historic). It does
		// not mutate when the functor only reads.
		const_cast<Cluster&>(cluster).cameras.forEachHandle(
			[&](ClusterCameraHandle h) { cameraHandles.push_back(h); });
	}
	for (uint32_t k = 0; k < cameraHandles.size(); ++k) {
		const auto h = cameraHandles[k];
		std::string uri = cluster.GetCameraUri(h).Str();
		if (uri.empty())
			uri = "<cluster-local>#camera." + std::to_string(h.index());
		auto idIt = camIdByUri.find(uri);
		if (idIt != camIdByUri.end()) {
			local.camera[h.index()] = idIt->second;
			continue;
		}
		const uint32_t pakId = static_cast<uint32_t>(camIdByUri.size());
		camIdByUri[uri] = pakId;
		local.camera[h.index()] = pakId;

		const SceneCamera* c = const_cast<Cluster&>(cluster).cameras.get(h);
		if (!c)
			return fail("stale camera handle in pool");

		std::vector<uint8_t> blob;
		PutStr(blob, uri);
		PutU32(blob, static_cast<uint32_t>(c->projection));
		PutF32(blob, c->fovYDeg);
		PutF32(blob, c->orthoHalfH);
		PutF32(blob, c->nearClip);
		PutF32(blob, c->farClip);
		PutStr(blob, c->name);

		if (!writer.AddBlob(PakAssetKind::Camera, uri, std::move(blob)))
			return fail("invalid camera uri: " + uri);
	}

	// --- Scene ---------------------------------------------------------------
	{
		const Scene& scene = cluster.scene;
		std::vector<uint8_t> blob;
		const uint32_t nodeCount = static_cast<uint32_t>(scene.hierarchy.size());
		PutU32(blob, nodeCount);
		for (const Hierarchy& h : scene.hierarchy) {
			PutI32(blob, h.parent);
			PutI32(blob, h.firstChild);
			PutI32(blob, h.nextSibling);
			PutI32(blob, h.lastSibling);
			PutI32(blob, h.level);
		}
		for (const glm::mat4& m : scene.localTransform) {
			const float* p = glm::value_ptr(m);
			for (int i = 0; i < 16; ++i)
				PutF32(blob, p[i]);
		}
		// meshForNode/materialForNode reference pool handle indices;
		// translate to pak-local ids.
		PutU32(blob, static_cast<uint32_t>(scene.meshForNode.size()));
		// Deterministic order for byte-stable paks.
		std::vector<std::pair<uint32_t, uint32_t>> meshPairs(
			scene.meshForNode.begin(), scene.meshForNode.end());
		std::sort(meshPairs.begin(), meshPairs.end());
		for (const auto& [node, localIdx] : meshPairs) {
			const auto it = local.mesh.find(localIdx);
			if (it == local.mesh.end())
				return fail("scene references unknown mesh pool index " + std::to_string(localIdx));
			PutU32(blob, node);
			PutU32(blob, it->second);
		}
		PutU32(blob, static_cast<uint32_t>(scene.materialForNode.size()));
		std::vector<std::pair<uint32_t, uint32_t>> matPairs(
			scene.materialForNode.begin(), scene.materialForNode.end());
		std::sort(matPairs.begin(), matPairs.end());
		for (const auto& [node, localIdx] : matPairs) {
			const auto it = local.mat.find(localIdx);
			if (it == local.mat.end())
				return fail("scene references unknown material pool index " + std::to_string(localIdx));
			PutU32(blob, node);
			PutU32(blob, it->second);
		}
		PutU32(blob, static_cast<uint32_t>(scene.nameForNode.size()));
		std::vector<std::pair<uint32_t, uint32_t>> namePairs(
			scene.nameForNode.begin(), scene.nameForNode.end());
		std::sort(namePairs.begin(), namePairs.end());
		for (const auto& [node, nameIdx] : namePairs) {
			PutU32(blob, node);
			PutU32(blob, nameIdx);
		}
		PutU32(blob, static_cast<uint32_t>(scene.nodeNames.size()));
		for (const auto& n : scene.nodeNames)
			PutStr(blob, n);
		PutU32(blob, static_cast<uint32_t>(scene.materialNames.size()));
		for (const auto& n : scene.materialNames)
			PutStr(blob, n);
		// Trailing static-subtree section (v2; v1 blobs end here).
		{
			PutU32(blob, nodeCount);
			if (scene.staticRoot.size() == scene.hierarchy.size()) {
				blob.insert(blob.end(), scene.staticRoot.begin(), scene.staticRoot.end());
				if (stats) {
					for (uint8_t v : scene.staticRoot)
						if (v)
							stats->staticRoots++;
				}
			}
			else {
				blob.insert(blob.end(), nodeCount, 0);
			}
		}
		// Trailing lightmap-exclusion section (v3; v2 blobs end above).
		// All-zero when nothing is excluded; v2 readers reject v3 blobs
		// (trailing-bytes check) — App/Editor ship together, so paks and
		// readers always move in lockstep.
		{
			PutU32(blob, nodeCount);
			if (scene.noLightmap.size() == scene.hierarchy.size()) {
				blob.insert(blob.end(), scene.noLightmap.begin(), scene.noLightmap.end());
			}
			else {
				blob.insert(blob.end(), nodeCount, 0);
			}
		}
		// Trailing cooked-lights section (v4; v3 blobs end above).
		// Definitions are PakAssetKind::Light blobs; this section carries
		// only the node links as (node, lightPakId) pairs in node order —
		// the same pak-id indirection meshPairs/matPairs use. Always
		// written (possibly empty) so the layout is uniform; v3 paks end
		// after the lightmap section (no lights).
		{
			std::vector<std::pair<uint32_t, uint32_t>> lightPairs(
				scene.lightForNode.begin(), scene.lightForNode.end());
			std::sort(lightPairs.begin(), lightPairs.end());
			PutU32(blob, static_cast<uint32_t>(lightPairs.size()));
			for (const auto& [node, localIdx] : lightPairs) {
				const auto it = local.light.find(localIdx);
				if (it == local.light.end())
					return fail("scene references unknown light pool index " +
						std::to_string(localIdx));
				PutU32(blob, node);
				PutU32(blob, it->second);
			}
		}
		// Trailing scene-name section (v5; v4 blobs end after the lights
		// section with an exact-end check — see the read side). Always
		// written (possibly empty) so the layout is uniform; discovery
		// (ListPakScenes) reads it without building a cluster.
		PutStr(blob, ci < sceneNames.size() ? sceneNames[ci] : std::string{});
		// Per-node dither-on-closeup section (v6; v5 blobs end after the
		// name). All-zero when unused; missing == off on read.
		{
			PutU32(blob, nodeCount);
			if (scene.ditherCloseup.size() == scene.hierarchy.size()) {
				blob.insert(blob.end(), scene.ditherCloseup.begin(),
					scene.ditherCloseup.end());
			}
			else {
				blob.insert(blob.end(), nodeCount, 0);
			}
		}
		// Per-node visibility section (v7; v6 blobs end after dither).
		// hidden flags (1 = hidden subtree root); all-zero == visible.
		// Missing == visible on read.
		{
			PutU32(blob, nodeCount);
			if (scene.hidden.size() == scene.hierarchy.size()) {
				blob.insert(blob.end(), scene.hidden.begin(),
					scene.hidden.end());
			}
			else {
				blob.insert(blob.end(), nodeCount, 0);
			}
		}
		// Per-node camera links (v8; v7 blobs end after visibility).
		// Definitions are PakAssetKind::Camera blobs; this section
		// carries only the node links as (node, camPakId) pairs in node
		// order — the same pak-id indirection the lights section uses.
		// Always written (possibly empty) so the layout is uniform.
		{
			std::vector<std::pair<uint32_t, uint32_t>> camPairs(
				scene.cameraForNode.begin(), scene.cameraForNode.end());
			std::sort(camPairs.begin(), camPairs.end());
			PutU32(blob, static_cast<uint32_t>(camPairs.size()));
			for (const auto& [node, localIdx] : camPairs) {
				const auto it = local.camera.find(localIdx);
				if (it == local.camera.end())
					return fail("scene references unknown camera pool index " +
						std::to_string(localIdx));
				PutU32(blob, node);
				PutU32(blob, it->second);
			}
		}

		if (!writer.AddBlob(PakAssetKind::Scene,
				SceneTocPath(ci, multi), std::move(blob)))
			return fail("scene blob rejected");
		if (stats)
			stats->nodes += nodeCount;
	}
	} // per-cluster loop

	// Cooked GI registry (pak-level, one blob): union of the clusters'
	// named sidecar bindings, first-seen order. Loaders enforce one
	// binding per kind for now (first wins + warn); the blob simply
	// lists what the cook registered.
	{
		std::vector<GiBinding> unionBindings;
		for (const Cluster* cp : clusters) {
			for (const GiBinding& b : cp->giBindings) {
				if (b.name.empty())
					continue;
				bool seen = false;
				for (const GiBinding& u : unionBindings)
					if (u.kind == b.kind && u.name == b.name) {
						seen = true;
						break;
					}
				if (!seen)
					unionBindings.push_back(b);
			}
		}
		if (!unionBindings.empty()) {
			std::vector<uint8_t> blob;
			PutU32(blob, static_cast<uint32_t>(unionBindings.size()));
			for (const GiBinding& b : unionBindings) {
				PutU32(blob, static_cast<uint32_t>(b.kind));
				PutStr(blob, b.name);
			}
			if (!writer.AddBlob(PakAssetKind::Gi, "<cluster-local>#gi",
					std::move(blob)))
				return fail("gi registry blob rejected");
			if (stats)
				stats->giBindings = static_cast<uint32_t>(unionBindings.size());
		}
	}

	if (stats) {
		stats->meshes = static_cast<uint32_t>(meshIdByUri.size());
		stats->materials = static_cast<uint32_t>(matIdByUri.size());
		stats->textures = static_cast<uint32_t>(texturesDone.size());
		stats->textureBytes = static_cast<uint32_t>(textureBytes);
		stats->lights = static_cast<uint32_t>(lightIdByUri.size());
		stats->cameras = static_cast<uint32_t>(camIdByUri.size());
	}
	return true;
}

bool SerializeCluster(
	const Cluster& cluster,
	PakWriter& writer,
	ClusterSerializeStats* stats,
	std::string* err) {
	return SerializeClusters({ &cluster }, {}, writer, stats, err);
}

// ---------------------------------------------------------------------------
// Deserialize
// ---------------------------------------------------------------------------

namespace {
// Pak-id remap tables for one scene parse (null = discovery listing:
// links are skipped but the cursor still walks + validates structure).
struct SceneLinkMaps {
	const std::vector<ClusterStaticMeshHandle>* meshes = nullptr;
	const std::vector<ClusterMaterialHandle>* materials = nullptr;
	const std::vector<ClusterLightHandle>* lights = nullptr;
	const std::vector<ClusterCameraHandle>* cameras = nullptr;
};

// Shared Scene-blob parse (see the v2/v3/v4/v5 trailing sections in the
// writer): hierarchy + transforms + node links + names + static/noLM/
// lights sections + v5 name. With maps, links resolve to fresh pool
// handles; without (ListPakScenes), only structure + names come out.
bool ParseSceneBlob(std::span<const uint8_t> payload, Scene& scene,
	std::string& nameOut, const SceneLinkMaps* maps, std::string& err) {
	auto fail = [&](const std::string& msg) {
		err = msg;
		return false;
	};
	Cursor c{ payload, 0 };
	const uint32_t nodeCount = c.u32();
	if (!c.ok || nodeCount == 0 || nodeCount > 1024 * 1024)
		return fail("bad scene node count");
	// Stash the recorded linkage first (single parse), then grow the
	// scene in index order with addNode (sibling order preserved), then
	// restore the recorded linkage verbatim as authoritative.
	std::vector<Hierarchy> recorded;
	recorded.reserve(nodeCount);
	for (uint32_t i = 0; i < nodeCount; ++i) {
		Hierarchy hh;
		hh.parent = c.i32();
		hh.firstChild = c.i32();
		hh.nextSibling = c.i32();
		hh.lastSibling = c.i32();
		hh.level = c.i32();
		if (!c.ok)
			return fail("truncated scene hierarchy");
		recorded.push_back(hh);
	}
	for (uint32_t i = 0; i < nodeCount; ++i)
		addNode(scene, recorded[i].parent, recorded[i].level);
	for (uint32_t i = 0; i < nodeCount; ++i)
		scene.hierarchy[i] = recorded[i];
	// Transforms.
	for (uint32_t i = 0; i < nodeCount; ++i) {
		glm::mat4 m(1.0f);
		float* p = glm::value_ptr(m);
		for (int k = 0; k < 16; ++k)
			p[k] = c.f32();
		if (!c.ok)
			return fail("truncated scene transforms");
		scene.localTransform[i] = m;
		scene.globalTransform[i] = glm::mat4(1.0f);
	}
	const uint32_t meshPairCount = c.u32();
	for (uint32_t i = 0; i < meshPairCount; ++i) {
		const uint32_t node = c.u32();
		const uint32_t pakId = c.u32();
		if (!c.ok || node >= nodeCount)
			return fail("bad scene mesh mapping");
		if (maps && maps->meshes) {
			if (pakId >= maps->meshes->size())
				return fail("bad scene mesh mapping");
			const auto h = (*maps->meshes)[pakId];
			if (!h.valid())
				return fail("scene references invalid mesh pak id");
			scene.meshForNode[node] = h.index();
		}
	}
	const uint32_t matPairCount = c.u32();
	for (uint32_t i = 0; i < matPairCount; ++i) {
		const uint32_t node = c.u32();
		const uint32_t pakId = c.u32();
		if (!c.ok || node >= nodeCount)
			return fail("bad scene material mapping");
		if (maps && maps->materials) {
			if (pakId >= maps->materials->size())
				return fail("bad scene material mapping");
			scene.materialForNode[node] = (*maps->materials)[pakId].index();
		}
	}
	const uint32_t namePairCount = c.u32();
	for (uint32_t i = 0; i < namePairCount; ++i) {
		const uint32_t node = c.u32();
		const uint32_t nameIdx = c.u32();
		if (!c.ok)
			return fail("bad scene name mapping");
		scene.nameForNode[node] = nameIdx;
	}
	const uint32_t nodeNameCount = c.u32();
	for (uint32_t i = 0; i < nodeNameCount; ++i)
		scene.nodeNames.push_back(c.str());
	const uint32_t matNameCount = c.u32();
	for (uint32_t i = 0; i < matNameCount; ++i)
		scene.materialNames.push_back(c.str());
	if (!c.ok)
		return fail("truncated scene blob");
	// Trailing static-subtree section (v2): u32 count + count bytes.
	// v1 blobs end exactly here (no static nodes).
	if (c.off < c.img.size()) {
		const uint32_t staticCount = c.u32();
		if (!c.ok || staticCount != nodeCount)
			return fail("bad scene static section");
		if (c.off + staticCount > c.img.size())
			return fail("scene blob trailing bytes mismatch");
		scene.staticRoot.assign(
			c.img.data() + c.off, c.img.data() + c.off + staticCount);
		c.off += staticCount;
	}
	else {
		scene.staticRoot.assign(nodeCount, 0);
	}
	// Trailing lightmap-exclusion section (v3): u32 count + count
	// bytes. v2 blobs end after the static section — default to no
	// exclusions in that case. v4 blobs carry a lights section after
	// this one, so only require that the bytes FIT (not exact end).
	if (c.off < c.img.size()) {
		const uint32_t lmCount = c.u32();
		if (!c.ok || lmCount != nodeCount)
			return fail("bad scene lightmap section");
		if (c.off + lmCount > c.img.size())
			return fail("scene blob trailing bytes mismatch");
		scene.noLightmap.assign(
			c.img.data() + c.off, c.img.data() + c.off + lmCount);
		c.off += lmCount;
	}
	else {
		scene.noLightmap.assign(nodeCount, 0);
	}
	// Trailing cooked-lights section (v4): u32 pairCount + (node,
	// lightPakId) pairs, remapped to fresh pool indices exactly like
	// the mesh/material mappings above. v3 blobs end after the
	// lightmap section (no lights).
	if (c.off < c.img.size()) {
		const uint32_t lightPairCount = c.u32();
		if (!c.ok || lightPairCount > 1024 * 1024)
			return fail("bad scene light mapping");
		for (uint32_t i = 0; i < lightPairCount; ++i) {
			const uint32_t node = c.u32();
			const uint32_t pakId = c.u32();
			if (!c.ok || node >= nodeCount)
				return fail("bad scene light mapping");
			if (maps && maps->lights) {
				if (pakId >= maps->lights->size())
					return fail("bad scene light mapping");
				const ClusterLightHandle h = (*maps->lights)[pakId];
				if (!h.valid())
					return fail("scene references invalid light pak id");
				scene.lightForNode[node] = h.index();
			}
		}
		if (!c.ok)
			return fail("truncated scene lights section");
		// Trailing scene-name section (v5; v4 blobs end after the
		// lights section — see the writer). Discovery reads it without
		// building a cluster.
		if (c.off < c.img.size()) {
			nameOut = c.str();
			if (!c.ok)
				return fail("truncated scene name");
		}
		// Per-node dither-on-closeup section (v6): u32 count + count
		// bytes. v5 blobs end after the name — default to off in that
		// case (and for the discovery listing path, which shares this
		// parse with null link maps).
		if (c.off < c.img.size()) {
			const uint32_t ditherCount = c.u32();
			if (!c.ok || ditherCount != nodeCount)
				return fail("bad scene dither section");
			if (c.off + ditherCount > c.img.size())
				return fail("scene blob trailing bytes mismatch");
			scene.ditherCloseup.assign(
				c.img.data() + c.off, c.img.data() + c.off + ditherCount);
			c.off += ditherCount;
		}
		else {
			scene.ditherCloseup.assign(nodeCount, 0);
		}
		// Per-node visibility section (v7): u32 count + hidden bytes.
		// v6 blobs end after dither — default to visible in that case.
		if (c.off < c.img.size()) {
			const uint32_t hidCount = c.u32();
			if (!c.ok || hidCount != nodeCount)
				return fail("bad scene visibility section");
			if (c.off + hidCount > c.img.size())
				return fail("scene blob trailing bytes mismatch");
			scene.hidden.assign(
				c.img.data() + c.off, c.img.data() + c.off + hidCount);
			c.off += hidCount;
		}
		else {
			scene.hidden.assign(nodeCount, 0);
		}
		// Per-node camera links (v8): u32 pairCount + (node, camPakId)
		// pairs, remapped exactly like lights. v7 blobs end after the
		// visibility section (no cameras).
		if (c.off < c.img.size()) {
			const uint32_t camPairCount = c.u32();
			if (!c.ok || camPairCount > 1024 * 1024)
				return fail("bad scene camera mapping");
			for (uint32_t i = 0; i < camPairCount; ++i) {
				const uint32_t node = c.u32();
				const uint32_t pakId = c.u32();
				if (!c.ok || node >= nodeCount)
					return fail("bad scene camera mapping");
				if (maps && maps->cameras) {
					if (pakId >= maps->cameras->size())
						return fail("bad scene camera mapping");
					const ClusterCameraHandle h = (*maps->cameras)[pakId];
					if (!h.valid())
						return fail("scene references invalid camera pak id");
					scene.cameraForNode[node] = h.index();
				}
			}
			if (!c.ok)
				return fail("truncated scene cameras section");
		}
		if (c.off != c.img.size())
			return fail("scene blob trailing bytes mismatch");
	}
	// Pre-v4 blobs skip the tail wholesale: normalize the v6/v7 defaults.
	if (scene.ditherCloseup.empty())
		scene.ditherCloseup.assign(nodeCount, 0);
	if (scene.hidden.empty())
		scene.hidden.assign(nodeCount, 0);
	RebuildStaticCache(scene);
	markAsChanged(scene, 0);
	recalculateGlobalTransforms(scene);
	return true;
}

// Scene blobs in index order ("#scene" first, then "#scene.N").
std::vector<const PakTocEntry*> OrderedSceneEntries(const PakReader& reader) {
	auto sceneEntries = reader.BlobsOfKind(PakAssetKind::Scene);
	std::sort(sceneEntries.begin(), sceneEntries.end(),
		[](const PakTocEntry* a, const PakTocEntry* b) {
			const uint32_t ia = SceneBlobIndex(a->path);
			const uint32_t ib = SceneBlobIndex(b->path);
			if (ia != ib)
				return ia < ib;
			return a->path < b->path;
		});
	return sceneEntries;
}

} // namespace

// Cooked GI registry blob (u32 count | u32 kind | str name). smvk scope
// to match the header: shared by DeserializeCluster (registers into the
// loaded cluster) and inspection tools (no cluster needed).
bool ParseGiRegistryBlob(
	std::span<const uint8_t> payload,
	std::vector<GiBinding>& out,
	std::string& err) {
	Cursor c{ payload, 0 };
	const uint32_t count = c.u32();
	if (!c.ok || count > 1024) {
		err = "bad gi registry count";
		return false;
	}
	out.clear();
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t kind = c.u32();
		const std::string name = c.str();
		if (!c.ok || (kind != 0 && kind != 1) || name.empty() ||
			name.size() > 512) {
			err = "bad gi registry entry";
			return false;
		}
		out.push_back(GiBinding{
			static_cast<GiBinding::Kind>(kind), name });
	}
	if (!c.ok || c.off != payload.size()) {
		err = "gi registry trailing bytes mismatch";
		return false;
	}
	return true;
}

std::vector<PakSceneInfo> ListPakScenes(
	const PakReader& reader,
	std::string* err) {
	std::vector<PakSceneInfo> out;
	for (const PakTocEntry* e : OrderedSceneEntries(reader)) {
		Scene tmp;
		std::string name, perr;
		if (!ParseSceneBlob(reader.Payload(*e), tmp, name, nullptr, perr)) {
			if (err)
				*err = e->path + ": " + perr;
			return {};
		}
		out.push_back(PakSceneInfo{ name, e->path,
			static_cast<uint32_t>(tmp.hierarchy.size()) });
	}
	return out;
}

std::unique_ptr<Cluster> DeserializeCluster(
	ResourceManager& ctx,
	const PakReader& reader,
	std::string* err,
	uint32_t sceneIndex) {
	// Failure after textures/meshes were queued in the upload streamer
	// leaves transfer command buffers + graphics acquire barriers
	// referencing the partial cluster's VkImages/VkBuffers. The cluster
	// is destroyed on return, so drain the streamer first: flush submits
	// the batches while the images are still alive, wait retires them,
	// and only then may the holders be torn down. Without this the
	// deferred destroy runs immediately (no graphics submit yet at load
	// time) while the upload is still in flight -> VUID-vkDestroyImage-
	// 01000 + a driver AV in the next submitState's vkCmdPipelineBarrier2
	// on the dead VkImage (see Bistro pak fallback crash).
	auto fail = [&](const std::string& msg) -> std::unique_ptr<Cluster> {
		if (err)
			*err = msg;
		std::cerr << "DeserializeCluster: " << msg << std::endl;
		// Workers are joined inside LoadKtxTexturesParallelMemory, so no
		// concurrent streamer submits are in flight here; safe to drain.
		ctx.WaitForUploads();
		return nullptr;
	};

	auto cluster = std::make_unique<Cluster>(&ctx);

	// --- Textures first (materials resolve URIs against them) ---------------
	const auto texEntries = reader.BlobsOfKind(PakAssetKind::Texture);
	struct TexLoad {
		std::string uri; // full asset uri (== file part for textures)
		PakTexturePayload payload;
	};
	std::vector<TexLoad> texLoads;
	texLoads.reserve(texEntries.size());
	for (const PakTocEntry* e : texEntries) {
		PakTexturePayload p;
		if (!ParseTexturePayload(reader.Payload(*e), p))
			return fail("malformed texture payload: " + e->path);
		texLoads.push_back(TexLoad{ p.uri.empty() ? e->path : p.uri, p });
	}
	// Parallel decode+upload; payload spans borrow the reader image, which
	// outlives this call by contract.
	std::vector<KtxMemoryJob> jobs;
	jobs.reserve(texLoads.size());
	for (size_t i = 0; i < texLoads.size(); ++i) {
		const auto& t = texLoads[i];
		jobs.push_back(KtxMemoryJob{
			static_cast<int>(i),
			t.payload.ktxBytes.data(),
			t.payload.ktxBytes.size(),
			t.uri,
		});
	}
	ParallelTextureStats texStats;
	auto holders = LoadKtxTexturesParallelMemory(ctx, jobs, {}, &texStats);
	if (texStats.failed > 0)
		std::cerr << "DeserializeCluster: " << texStats.failed << " texture(s) failed to load" << std::endl;
	for (size_t i = 0; i < texLoads.size(); ++i) {
		const auto it = holders.find(static_cast<int>(i));
		if (it == holders.end())
			continue; // failed; materials referencing it fall back to empty
		const auto& t = texLoads[i];
		TextureHandle rhi = it->second;
		cluster->textures.create(std::move(it->second));
		cluster->RegisterTextureUri(t.uri, rhi);
		cluster->RegisterTextureSampler(t.uri, TextureSamplerDesc{
			t.payload.magFilter, t.payload.minFilter,
			t.payload.wrapS, t.payload.wrapT,
		});
		cluster->SetTextureSource(t.uri,
			std::vector<uint8_t>(t.payload.ktxBytes.begin(), t.payload.ktxBytes.end()));
	}

	// --- Materials ------------------------------------------------------------
	const auto matEntries = reader.BlobsOfKind(PakAssetKind::Material);
	std::vector<ClusterMaterialHandle> matByPakId;
	matByPakId.reserve(matEntries.size());
	for (const PakTocEntry* e : matEntries) {
		Cursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		// Material blob, strict: the CURRENT MaterialValues layout and
		// ALL trailing fields (texture URIs, castsShadows). Paks are
		// cooked artifacts of this build — older layouts fail loudly and
		// get recooked; no runtime tolerance of stale shapes.
		MaterialValues values{};
		if (c.off + sizeof(MaterialValues) > c.img.size())
			return fail("truncated material values (pak cooked with an "
				"older layout - recook the pak): " + e->path);
		memcpy(&values, c.img.data() + c.off, sizeof(MaterialValues));
		c.off += sizeof(MaterialValues);
		const uint32_t shading = c.u32();
		const uint32_t alpha = c.u32();
		const float cutoff = c.f32();
		const uint32_t ds = c.u32();
		std::string texUris[5];
		for (int k = 0; k < 5; ++k)
			texUris[k] = c.str();
		if (!c.ok)
			return fail("truncated material blob: " + e->path);
		// castsShadows trailer (after the texture URIs).
		const uint32_t cs = c.u32();
		if (!c.ok)
			return fail("truncated material blob (missing castsShadows; "
				"stale pak - recook the pak): " + e->path);
		const bool castsShadows = cs != 0;

		// Exact MaterialValues round-trip: everything (factors, params,
		// ambient) is preserved verbatim except the texture slots, which
		// are session-local RHI indices at cook time and must be remapped
		// to the freshly-uploaded handles.
		auto texSlot = [&](const std::string& u) -> int32_t {
			if (u.empty())
				return -1;
			auto hit = cluster->FindTextureByUri(u);
			return (hit && !hit->empty()) ? static_cast<int32_t>(hit->index()) : -1;
		};
		values.textures[0] = texSlot(texUris[0]);
		values.textures[1] = texSlot(texUris[1]);
		values.textures[2] = texSlot(texUris[2]);
		values.textures[3] = texSlot(texUris[3]);
		values.textures2[0] = texSlot(texUris[4]);

		const ClusterMaterialHandle h = cluster->CreateMaterialFromValues(
			values,
			shading <= 2 ? static_cast<ShadingModel>(shading) : ShadingModel::DefaultLit,
			alpha <= 2 ? static_cast<AlphaMode>(alpha) : AlphaMode::Opaque,
			cutoff,
			ds != 0,
			castsShadows);
		if (!h.valid())
			return fail("CreateMaterial failed for: " + uri);
		cluster->RegisterMaterialUri(uri, h);
		matByPakId.push_back(h);
	}

	// --- Meshes (vertex/index payloads re-uploaded via the standard path) --
	// Uploads are streamer-batched; the caller issues ctx.FlushUploads().
	const auto meshEntries = reader.BlobsOfKind(PakAssetKind::Mesh);
	std::vector<ClusterStaticMeshHandle> meshByPakId;
	meshByPakId.reserve(meshEntries.size());
	for (const PakTocEntry* e : meshEntries) {
		Cursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		const uint32_t vertexCount = c.u32();
		const uint32_t indexCount = c.u32();
		const uint32_t surfaceCount = c.u32();
		if (!c.ok || vertexCount == 0 || vertexCount > 16 * 1024 * 1024 ||
			indexCount > 64 * 1024 * 1024 || surfaceCount > 4096)
			return fail("truncated/insane mesh header: " + e->path);
		StaticMeshUpload upload;
		upload.surfaces.reserve(surfaceCount);
		for (uint32_t s = 0; s < surfaceCount; ++s) {
			Surface surf;
			surf.firstIndex = c.u32();
			surf.indexCount = c.u32();
			surf.baseVertex = c.i32();
			upload.surfaces.push_back(surf);
		}
		upload.aabbMin = c.vec3();
		upload.aabbMax = c.vec3();
		const float sphere = c.f32();
		const uint32_t topology = c.u32();
		if (topology != static_cast<uint32_t>(PrimitiveTopology::TriangleList))
			return fail("unsupported mesh topology in: " + e->path);
		LodInfo lod;
		lod.groupId = c.u32();
		lod.lodIndex = c.u32();
		lod.candidateCount = c.u32();
		lod.simplifyError = c.f32();
		lod.screenThreshold = c.f32();
		lod.triCount = c.u32();
		lod.acmr = c.f32();
		const uint32_t lmChunk = c.u32();
		const uint32_t lmXformCount = c.u32();
		std::vector<glm::vec4> lmXforms;
		if (!c.ok || lmXformCount > 1024 * 1024)
			return fail("truncated/corrupt mesh lightmap transforms: " + e->path);
		lmXforms.reserve(lmXformCount);
		for (uint32_t i = 0; i < lmXformCount; ++i) {
			glm::vec4 xf;
			xf.x = c.f32();
			xf.y = c.f32();
			xf.z = c.f32();
			xf.w = c.f32();
			if (!c.ok)
				return fail("truncated mesh lightmap transforms: " + e->path);
			lmXforms.push_back(xf);
		}
		if (!c.ok)
			return fail("truncated mesh metadata: " + e->path);
		// Vertex payload: the CURRENT StaticVertex layout, strict. Paks are
		// cooked artifacts of this engine build — a layout change means a
		// recook, never a runtime probe (silent tolerance of older vertex
		// layouts fed garbage into the downstream systems; a hard fail
		// names the problem and the fix).
		const size_t vertBytes = static_cast<size_t>(vertexCount) * sizeof(StaticVertex);
		const size_t idxBytes = static_cast<size_t>(indexCount) * sizeof(uint32_t);
		if (c.off + vertBytes + idxBytes > c.img.size())
			return fail("mesh payload size mismatch (pak cooked with an "
				"older vertex layout - recook the pak): " + e->path);
		upload.vertices.resize(vertexCount);
		memcpy(upload.vertices.data(), c.img.data() + c.off, vertBytes);
		c.off += vertBytes;
		upload.indices.resize(indexCount);
		memcpy(upload.indices.data(), c.img.data() + c.off,
			static_cast<size_t>(indexCount) * sizeof(uint32_t));
		c.off += static_cast<size_t>(indexCount) * sizeof(uint32_t);
		// Trailing meshlet section. The cooker ALWAYS writes it (possibly
		// count 0), so a missing or malformed trailer is a stale/corrupt
		// pak: hard fail. Meshlets are a COOKED artifact — the runtime
		// never rebuilds them; a mesh without cooked meshlets draws direct
		// (unculled) until the pak is recooked.
		CookedMeshlets cooked;
		{
			if (c.off >= c.img.size())
				return fail("mesh blob has no meshlet trailer (stale pak - "
					"recook the pak): " + e->path);
			const uint32_t meshletCount = c.u32();
			if (!c.ok || meshletCount > 1024 * 1024)
				return fail("corrupt meshlet trailer: " + e->path);
			cooked.meshlets.reserve(meshletCount);
			for (uint32_t m = 0; m < meshletCount; ++m) {
				Meshlet ml;
				ml.vertexOffset = c.u32();
				ml.vertexCount = c.u32();
				ml.triangleOffset = c.u32();
				ml.triangleCount = c.u32();
				ml.boundCenter = c.vec3();
				ml.boundRadius = c.f32();
				ml.coneApex = c.vec3();
				ml.coneAxis = c.vec3();
				ml.coneCutoff = c.f32();
				if (!c.ok || ml.vertexCount > 255 || ml.triangleCount > 512)
					return fail("corrupt meshlet trailer: " + e->path);
				cooked.meshlets.push_back(ml);
			}
			const uint32_t vertCount = c.u32();
			if (!c.ok || vertCount > 64 * 1024 * 1024)
				return fail("corrupt meshlet trailer: " + e->path);
			cooked.vertices.reserve(vertCount);
			for (uint32_t v = 0; v < vertCount; ++v)
				cooked.vertices.push_back(c.u32());
			const uint32_t triBytes = c.u32();
			if (!c.ok || triBytes > 256 * 1024 * 1024
				|| c.off + triBytes != c.img.size())
				return fail("corrupt meshlet trailer: " + e->path);
			cooked.triangles.assign(
				c.img.data() + c.off, c.img.data() + c.off + triBytes);
			// Cross-check windows against the payload sizes.
			for (const Meshlet& ml : cooked.meshlets) {
				if ((size_t)ml.vertexOffset + ml.vertexCount > cooked.vertices.size() ||
					(size_t)ml.triangleOffset + (size_t)ml.triangleCount * 3 > cooked.triangles.size()) {
					return fail("corrupt meshlet trailer: " + e->path);
				}
			}
		}

		const ClusterStaticMeshHandle h = cluster->UploadStaticMesh(upload);
		if (!h.valid())
			return fail("UploadStaticMesh failed for: " + uri);
		// UploadStaticMesh fills AABB only; restore the cooked sphere and
		// the lightmap chunk (rides the draw ring to the lit fragment).
		if (StaticMesh* m = cluster->staticMeshes.get(h)) {
			m->geometry.sphereRadius = sphere;
			m->lmChunk = lmChunk;
			m->lmChartXforms = std::move(lmXforms);
			if (!cooked.empty())
				m->meshlets = std::move(cooked);
		}
		cluster->RegisterMeshUri(uri, h);
		cluster->RegisterMeshLod(uri, lod);
		if (StaticMesh* m = cluster->staticMeshes.get(h)) {
			if (m->meshlets && !m->meshlets->empty())
				cluster->RegisterMeshMeshlets(uri, *m->meshlets);
		}
		meshByPakId.push_back(h);
	}

	// --- Lights (definitions; the Scene blob links nodes to pak ids) -------
	// Loaded exactly like meshes: one pool entry per blob, URI
	// re-registered, and the scene's (node, pakId) pairs below remap to
	// fresh pool handle indices.
	const auto lightEntries = reader.BlobsOfKind(PakAssetKind::Light);
	std::vector<ClusterLightHandle> lightByPakId;
	lightByPakId.reserve(lightEntries.size());
	for (const PakTocEntry* e : lightEntries) {
		Cursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		const uint32_t type = c.u32();
		const glm::vec3 color = c.vec3();
		const float intensity = c.f32();
		const float range = c.f32();
		const float inner = c.f32();
		const float outer = c.f32();
		const std::string name = c.str();
		if (!c.ok)
			return fail("truncated light blob: " + e->path);
		if (type > 2)
			return fail("bad light type in: " + e->path);
		// Trailing flags section (v5): kSceneLightFlag_* (static = bake
		// source). v4 blobs end after the name — default to 0.
		uint32_t lightFlags = 0;
		if (c.off + 4 <= c.img.size())
			lightFlags = c.u32();
		if (!c.ok)
			return fail("truncated light flags: " + e->path);
		SceneLight l;
		l.type = static_cast<SceneLight::Type>(type);
		l.color = color;
		l.intensity = intensity;
		l.range = range;
		l.innerConeAngle = inner;
		l.outerConeAngle = outer;
		l.name = name;
		l.flags = lightFlags;
		const ClusterLightHandle h = cluster->CreateLight(std::move(l));
		if (!h.valid())
			return fail("CreateLight failed for: " + uri);
		cluster->RegisterLightUri(uri, h);
		lightByPakId.push_back(h);
	}

	// --- Cameras (definitions; the Scene blob links nodes to pak ids) ----
	// Loaded exactly like lights: one pool entry per blob, URI
	// re-registered, and the scene's (node, pakId) pairs below remap to
	// fresh pool handle indices.
	const auto cameraEntries = reader.BlobsOfKind(PakAssetKind::Camera);
	std::vector<ClusterCameraHandle> camByPakId;
	camByPakId.reserve(cameraEntries.size());
	for (const PakTocEntry* e : cameraEntries) {
		Cursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		const uint32_t proj = c.u32();
		const float fovY = c.f32();
		const float halfH = c.f32();
		const float nearClip = c.f32();
		const float farClip = c.f32();
		const std::string name = c.str();
		if (!c.ok)
			return fail("truncated camera blob: " + e->path);
		if (proj > 1)
			return fail("bad camera projection in: " + e->path);
		if (c.off != c.img.size())
			return fail("trailing bytes in camera blob: " + e->path);
		SceneCamera cam;
		cam.projection = (proj == 1)
			? SceneCamera::Projection::Orthographic
			: SceneCamera::Projection::Perspective;
		cam.fovYDeg = fovY;
		cam.orthoHalfH = halfH;
		cam.nearClip = nearClip;
		cam.farClip = farClip;
		cam.name = name;
		const ClusterCameraHandle h = cluster->CreateCamera(std::move(cam));
		if (!h.valid())
			return fail("CreateCamera failed for: " + uri);
		cluster->RegisterCameraUri(uri, h);
		camByPakId.push_back(h);
	}

	// --- Cooked GI registry (named sidecars; 0..1 blobs, first wins) -----
	for (const PakTocEntry* e : reader.BlobsOfKind(PakAssetKind::Gi)) {
		std::vector<GiBinding> bindings;
		std::string gerr;
		if (!ParseGiRegistryBlob(reader.Payload(*e), bindings, gerr))
			return fail("gi registry rejected: " + gerr);
		for (const GiBinding& b : bindings)
			cluster->RegisterGiBinding(b.kind, b.name);
		break; // first registry wins; the writer emits one
	}

	// --- Scene (indexed; 0 = default) --------------------------------------
	const auto sceneEntries = OrderedSceneEntries(reader);
	if (sceneEntries.empty())
		return fail("pak contains no Scene blob");
	if (sceneIndex >= sceneEntries.size())
		return fail("scene index " + std::to_string(sceneIndex) +
			" out of range (" + std::to_string(sceneEntries.size()) +
			" scene(s) in pak)");
	{
		SceneLinkMaps maps{ &meshByPakId, &matByPakId, &lightByPakId,
			&camByPakId };
		std::string sceneName, perr;
		if (!ParseSceneBlob(reader.Payload(*sceneEntries[sceneIndex]),
				cluster->scene, sceneName, &maps, perr))
			return fail(perr);
		if (!sceneName.empty())
			std::cerr << "DeserializeCluster: scene[" << sceneIndex
				<< "] '" << sceneName << "'\n";
	}
	return cluster;
}

} // namespace smvk
