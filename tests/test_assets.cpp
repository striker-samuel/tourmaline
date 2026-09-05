// asset_tests: headless unit tests for VFS + pak container + asset URIs +
// the meshopt cook pass. No GPU required (nothing here constructs a
// ResourceManager); run via CTest.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <Engine/App/cluster.h>
#include <Engine/App/static_merge.h>
#include <Engine/Asset/asset_registry.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Asset/pak.h>
#include <Engine/Scene/scene.h>
#include <Engine/Vfs/vfs.h>

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

void TestVfsNormalize() {
	using smvk::vfs::Normalize;
	Check(Normalize("models/Sponza/sponza.gltf") == "models/Sponza/sponza.gltf", "vfs.norm.plain");
	Check(Normalize("models\\Sponza\\sponza.gltf") == "models/Sponza/sponza.gltf", "vfs.norm.backslash");
	Check(Normalize("./models//Sponza/./sponza.gltf") == "models/Sponza/sponza.gltf", "vfs.norm.dots");
	Check(Normalize("models/Sponza/../Sponza/sponza.gltf") == "models/Sponza/sponza.gltf", "vfs.norm.dotdot");
	Check(Normalize("") == "", "vfs.norm.empty");
	Check(Normalize("/absolute/path") == "", "vfs.norm.absolute");
	Check(Normalize("C:/win/path") == "", "vfs.norm.drive");
	Check(Normalize("../escape") == "", "vfs.norm.escape");
	Check(Normalize("a/../../escape") == "", "vfs.norm.escape2");
	Check(smvk::vfs::Join("models/Sponza", "sponza.gltf") == "models/Sponza/sponza.gltf", "vfs.join");
	Check(smvk::vfs::BaseDir("models/Sponza/sponza.gltf") == "models/Sponza", "vfs.basedir");
	Check(smvk::vfs::FileName("models/Sponza/sponza.gltf") == "sponza.gltf", "vfs.filename");
}

void TestOverlayMem() {
	smvk::OverlayVfs vfs;
	auto base = std::make_shared<smvk::MemBackend>("base");
	auto over = std::make_shared<smvk::MemBackend>("over");
	base->Write("textures/a.ktx2", { 1, 2, 3 });
	base->Write("textures/b.ktx2", { 4, 5 });
	over->Write("textures/a.ktx2", { 9, 9, 9, 9 });
	vfs.Mount(base, 10, "base");
	vfs.Mount(over, 0, "over"); // higher precedence

	auto a = vfs.TryRead("textures/a.ktx2");
	Check(a && a->size() == 4 && (*a)[0] == 9, "vfs.overlay.precedence");
	auto b = vfs.TryRead("textures/b.ktx2");
	Check(b && b->size() == 2, "vfs.overlay.fallthrough");
	Check(!vfs.Exists("textures/missing.ktx2"), "vfs.overlay.miss");
	auto providers = vfs.FindProviders("textures/a.ktx2");
	Check(providers.size() == 2, "vfs.overlay.providers");
	auto dir = vfs.ListDir("textures");
	Check(dir.size() == 2, "vfs.overlay.listdir");
	bool threw = false;
	try {
		vfs.ReadBinary("nope.bin");
	}
	catch (const std::runtime_error&) {
		threw = true;
	}
	Check(threw, "vfs.overlay.throw");
	Check(vfs.Unmount("over"), "vfs.unmount");
	auto a2 = vfs.TryRead("textures/a.ktx2");
	Check(a2 && a2->size() == 3, "vfs.unmount.fallback");
}

void TestAssetUris() {
	auto [f, frag] = smvk::SplitAssetUri("models/s.gltf#mesh.1/prim.0");
	Check(f == "models/s.gltf" && frag == "mesh.1/prim.0", "uri.split");
	Check(smvk::NormalizeAssetUri("models\\s.gltf#mesh.1") == "models/s.gltf#mesh.1", "uri.normalize");
	Check(smvk::NormalizeAssetUri("../evil#x") == "", "uri.escape");
	Check(smvk::MakeSubUri("models/s.gltf", "mat.3") == "models/s.gltf#mat.3", "uri.mksub");
	Check(smvk::AssetFilePart("models/s.gltf#mat.3") == "models/s.gltf", "uri.filepart");
}

void TestPakRoundTrip() {
	smvk::PakWriter w;
	Check(!w.AddBlob(smvk::PakAssetKind::Texture, "../evil", { 1 }), "pak.reject.escape");
	Check(w.AddBlob(smvk::PakAssetKind::Texture, "textures/a.ktx2", { 1, 2, 3, 4 }), "pak.add.tex");
	std::vector<uint8_t> scene = { 9, 9, 9 };
	Check(w.AddBlob(smvk::PakAssetKind::Scene, "<cluster-local>#scene", scene), "pak.add.scene");
	std::vector<uint8_t> img = w.Finalize();
	Check(img.size() > 64, "pak.finalize.size");

	std::string err;
	auto r = smvk::PakReader::Open(std::span<const uint8_t>(img), err);
	Check(r.has_value(), "pak.open.ok");
	if (!r) {
		std::cout << "  pak error: " << err << "\n";
		return;
	}
	Check(r->EntryCount() == 2, "pak.entry.count");
	const auto* tex = r->Find(smvk::PakAssetKind::Texture, "textures/a.ktx2");
	Check(tex != nullptr, "pak.find.tex");
	if (tex) {
		auto payload = r->Payload(*tex);
		Check(payload.size() == 4 && payload[0] == 1, "pak.payload");
	}
	Check(r->Find(smvk::PakAssetKind::Mesh, "textures/a.ktx2") == nullptr, "pak.find.kindmiss");

	// Corruption is detected.
	std::vector<uint8_t> bad = img;
	bad[40] ^= 0xFF;
	std::string err2;
	Check(!smvk::PakReader::Open(std::span<const uint8_t>(bad), err2), "pak.corrupt.detected");

	// Texture payload helpers.
	auto tp = smvk::BuildTexturePayload("textures/a.ktx2", 1, 2, 3, 4,
		std::span<const uint8_t>(std::vector<uint8_t>{ 7, 8 }));
	smvk::PakTexturePayload parsed;
	Check(smvk::ParseTexturePayload(tp, parsed), "pak.texpayload.parse");
	Check(parsed.uri == "textures/a.ktx2" && parsed.magFilter == 1 &&
		parsed.minFilter == 2 && parsed.wrapS == 3 && parsed.wrapT == 4 &&
		parsed.ktxBytes.size() == 2 && parsed.ktxBytes[0] == 7, "pak.texpayload.fields");
}

void TestPakBackendServesTextures() {
	smvk::PakWriter w;
	auto tp = smvk::BuildTexturePayload("textures/brick.ktx2", -1, -1, -1, -1,
		std::span<const uint8_t>(std::vector<uint8_t>{ 10, 20, 30 }));
	w.AddBlob(smvk::PakAssetKind::Texture, "textures/brick.ktx2", std::move(tp));
	std::vector<uint8_t> scene = { 1 };
	w.AddBlob(smvk::PakAssetKind::Scene, "<cluster-local>#scene", scene);
	auto img = std::make_shared<const std::vector<uint8_t>>(w.Finalize());
	std::string err;
	auto backend = smvk::PakBackend::Open(img, "test.pak", err);
	Check(backend != nullptr, "pakbackend.open");
	if (!backend) {
		std::cout << "  backend error: " << err << "\n";
		return;
	}
	smvk::OverlayVfs vfs;
	vfs.Mount(backend, 0, "test.pak");
	auto bytes = vfs.TryRead("textures/brick.ktx2");
	Check(bytes && bytes->size() == 3 && (*bytes)[0] == 10, "pakbackend.serve.ktx");
	Check(!vfs.Exists("models/s.gltf"), "pakbackend.nontex.invisible");
	auto dir = vfs.ListDir("textures");
	Check(dir.size() == 1 && dir[0] == "textures/brick.ktx2", "pakbackend.listdir");
}

smvk::StaticMeshUpload MakeQuad() {
	smvk::StaticMeshUpload u;
	for (int i = 0; i < 4; ++i) {
		smvk::StaticVertex v{};
		v.pos = { (i & 1) ? 1.0f : 0.0f, (i & 2) ? 1.0f : 0.0f, 0.0f };
		v.color = { 1, 1, 1 };
		v.texCoord = { v.pos.x, v.pos.y };
		v.normal = { 0, 0, 1 };
		v.tangent = { 1, 0, 0, 1 };
		u.vertices.push_back(v);
	}
	u.indices = { 0, 1, 2, 0, 2, 3 };
	u.aabbMin = { 0, 0, 0 };
	u.aabbMax = { 1, 1, 0 };
	return u;
}

void TestMeshOpt() {
	smvk::MeshOptConfig cfg; // all passes on
	auto quad = MakeQuad();
	smvk::MeshOptStats stats;
	smvk::LodInfo lod;
	Check(smvk::OptimizeStaticMeshUpload(quad, cfg, 7, &stats, &lod), "meshopt.quad.ok");
	Check(quad.vertices.size() == 4 && quad.indices.size() == 6, "meshopt.quad.topology");
	Check(stats.triangles == 2 && stats.lodGroupId == 7, "meshopt.quad.stats");
	Check(stats.acmrAfter > 0.0f && stats.acmrAfter <= stats.acmrBefore + 1e-5f, "meshopt.quad.acmr");
	Check(lod.lodIndex == 0 && lod.screenThreshold < 0.0f, "meshopt.lod.unplugged");
	Check(lod.triCount == 2, "meshopt.lod.tris");

	// Weld merges duplicated vertices.
	auto dup = MakeQuad();
	dup.vertices.push_back(dup.vertices[0]);
	dup.vertices.push_back(dup.vertices[1]);
	dup.vertices.push_back(dup.vertices[2]);
	dup.indices = { 4, 5, 6, 0, 2, 3 };
	smvk::MeshOptStats ws;
	Check(smvk::OptimizeStaticMeshUpload(dup, cfg, 0, &ws, nullptr), "meshopt.weld.ok");
	Check(ws.outVertices == 4, "meshopt.weld.merged");

	// Degenerate input fails softly.
	smvk::StaticMeshUpload empty;
	smvk::LodInfo elod;
	Check(!smvk::OptimizeStaticMeshUpload(empty, cfg, 0, nullptr, &elod), "meshopt.empty.false");
	Check(elod.lodIndex == 0, "meshopt.empty.lodmarker");

	// Disabled config leaves data untouched.
	auto q2 = MakeQuad();
	smvk::MeshOptConfig off;
	off.enabled = false;
	Check(!smvk::OptimizeStaticMeshUpload(q2, off, 0), "meshopt.disabled.false");
	Check(q2.vertices.size() == 4, "meshopt.disabled.untouched");
}

void TestClusterSerializeCook() {
	using namespace smvk;
	// GPU-free cook cluster: CPU geometry + minted texture ids only.
	auto cluster = std::make_unique<Cluster>(nullptr);
	cluster->SetCookMode(true);
	Check(cluster->IsCookMode(), "cook.mode.on");

	// Mesh (cook path retains uploads for serialization).
	auto quad = MakeQuad();
	const ClusterStaticMeshHandle mh = cluster->UploadStaticMesh(quad);
	Check(mh.valid(), "cook.mesh.handle");
	Check(cluster->GetRetainedUpload(mh) != nullptr, "cook.mesh.retained");
	Check(cluster->GetRetainedUpload(mh)->vertices.size() == 4, "cook.mesh.retained.verts");
	const std::string meshUri = "models/q.gltf#mesh.0/prim.0";
	Check(cluster->RegisterMeshUri(meshUri, mh), "cook.mesh.uri");
	{
		LodInfo lod;
		lod.triCount = 2;
		cluster->RegisterMeshLod(meshUri, lod);
	}

	// Texture: minted handle (no RHI pool), validated payload stashed.
	const TextureHandle th = TextureHandle::Mint(cluster->AllocCookTextureId());
	const std::string texUri = "textures/fake.ktx2";
	Check(th.valid() && th.index() == 0, "cook.tex.mint");
	Check(cluster->RegisterTextureUri(texUri, th), "cook.tex.uri");
	cluster->RegisterTextureSampler(texUri, TextureSamplerDesc{ 9729, 9987, 10497, 10497 });
	// Minimal fake KTX2 payload (magic + pad) — serializer embeds verbatim.
	std::vector<uint8_t> fakeKtx = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
		0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4 };
	cluster->SetTextureSource(texUri, fakeKtx);
	Check(cluster->GetTextureSource(texUri) != nullptr, "cook.tex.src");

	// Material referencing the minted texture slot.
	MaterialCreateInfo ci{};
	ci.baseColorTexture = th;
	ci.metallicFactor = 0.25f;
	ci.roughnessFactor = 0.75f;
	ci.shadingModel = ShadingModel::Masked;
	ci.alphaMode = AlphaMode::Mask;
	ci.alphaCutoff = 0.33f;
	ci.doubleSided = true;
	const ClusterMaterialHandle math = cluster->CreateMaterial(ci);
	Check(math.valid(), "cook.mat.handle");
	Check(cluster->RegisterMaterialUri("models/q.gltf#mat.0", math), "cook.mat.uri");
	Check(cluster->FindMaterialByUri("models/q.gltf#mat.0") == math, "cook.mat.find");
	{
		const Material* m = cluster->materials.get(math);
		Check(m && m->values.textures[0] == 0 && m->doubleSided &&
			m->alphaMode == AlphaMode::Mask, "cook.mat.values");
	}

	// Scene: root + one renderable node.
	const int root = addNode(cluster->scene, -1, 0);
	const int child = addNode(cluster->scene, root, 1);
	cluster->scene.meshForNode[child] = mh.index();
	cluster->scene.materialForNode[child] = math.index();
	setNodeName(cluster->scene, child, "QuadNode");

	PakWriter writer;
	ClusterSerializeStats stats;
	std::string err;
	Check(SerializeCluster(*cluster, writer, &stats, &err), "cook.serialize.ok");
	if (!err.empty())
		std::cout << "  serialize err: " << err << "\n";
	Check(stats.meshes == 1 && stats.materials == 1 && stats.textures == 1 &&
		stats.nodes == 2, "cook.serialize.stats");

	std::vector<uint8_t> img = writer.Finalize();
	std::string openErr;
	auto reader = PakReader::Open(std::span<const uint8_t>(img), openErr);
	Check(reader.has_value(), "cook.pak.open");
	if (!reader)
		return;
	Check(reader->BlobsOfKind(PakAssetKind::Mesh).size() == 1, "cook.pak.mesh1");
	Check(reader->BlobsOfKind(PakAssetKind::Material).size() == 1, "cook.pak.mat1");
	Check(reader->BlobsOfKind(PakAssetKind::Texture).size() == 1, "cook.pak.tex1");
	Check(reader->BlobsOfKind(PakAssetKind::Scene).size() == 1, "cook.pak.scene1");

	// Texture payload round-trip (bytes + sampler).
	const PakTocEntry* tex = reader->Find(PakAssetKind::Texture, texUri);
	Check(tex != nullptr, "cook.pak.tex.find");
	if (tex) {
		PakTexturePayload p;
		Check(ParseTexturePayload(reader->Payload(*tex), p), "cook.pak.tex.parse");
		Check(p.uri == texUri && p.magFilter == 9729 && p.minFilter == 9987 &&
			p.wrapS == 10497 && p.wrapT == 10497, "cook.pak.tex.sampler");
		Check(p.ktxBytes.size() == fakeKtx.size() &&
			memcmp(p.ktxBytes.data(), fakeKtx.data(), fakeKtx.size()) == 0,
			"cook.pak.tex.bytes");
	}

	// Mesh blob contains the retained vertex/index payload.
	const auto meshes = reader->BlobsOfKind(PakAssetKind::Mesh);
	Check(!meshes.empty() &&
		reader->Payload(*meshes[0]).size() ==
			4 * sizeof(StaticVertex) + 6 * sizeof(uint32_t) + (0u /*header*/) +
			// header: uri len+bytes, counts, surfaces(0), aabb, sphere, topo, lod
			4 + meshUri.size() + 4 + 4 + 4 + 0 + 24 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 +
			// trailing meshlet section (v2, empty here): count + verts + tris
			4 + 4 + 4,
		"cook.pak.mesh.payload");
}

void TestStaticSubtrees() {
	Scene scene;
	const int root = addNode(scene, -1, 0);
	const int a = addNode(scene, root, 1);
	const int b = addNode(scene, a, 2);
	const int c = addNode(scene, root, 1);
	Check(!IsStaticNode(scene, a), "static.init.dynamic");
	MarkSubtreeStatic(scene, a);
	Check(IsStaticSubtreeRoot(scene, a), "static.root");
	Check(IsStaticNode(scene, a) && IsStaticNode(scene, b), "static.cache.desc");
	Check(!IsStaticNode(scene, root) && !IsStaticNode(scene, c), "static.cache.sibling");
	// Dynamic dirty path skips frozen nodes.
	markAsChangedDynamic(scene, root);
	bool sawStatic = false;
	for (int l = 0; l < MAX_NODE_LEVEL; ++l)
		for (int n : scene.changedAtThisFrame[l])
			if (n == a || n == b)
				sawStatic = true;
	Check(!sawStatic, "static.dirty.skip");
	for (int l = 0; l < MAX_NODE_LEVEL; ++l)
		scene.changedAtThisFrame[l].clear();
	// Dynamic recalc leaves baked globals alone.
	scene.globalTransform[b] = glm::mat4(2.0f);
	markAsChanged(scene, root);
	Check(recalculateGlobalTransformsDynamic(scene), "static.recalc.dynamic");
	Check(scene.globalTransform[b] == glm::mat4(2.0f), "static.recalc.baked");
	ClearSubtreeStatic(scene, a);
	Check(!IsStaticNode(scene, a) && !IsStaticNode(scene, b), "static.clear");
}

void TestMeshlets() {
	smvk::MeshletConfig cfg;
	cfg.enabled = true;
	cfg.maxVertices = 64;
	cfg.maxTriangles = 124;
	auto quad = MakeQuad();
	smvk::CookedMeshlets cooked;
	smvk::MeshletStats st;
	Check(smvk::BuildMeshletsForUpload(quad, cfg, &cooked, &st, nullptr), "meshlet.quad.ok");
	Check(!cooked.empty() && st.meshletCount >= 1, "meshlet.quad.count");
	Check(st.totalTriangles == 2, "meshlet.quad.tris");
	bool windowsOk = true;
	for (const auto& m : cooked.meshlets) {
		if (m.vertexCount == 0 || m.vertexCount > 64 || m.triangleCount == 0 ||
			m.triangleCount > 124 || m.boundRadius <= 0.0f)
			windowsOk = false;
		if ((size_t)m.vertexOffset + m.vertexCount > cooked.vertices.size() ||
			(size_t)m.triangleOffset + (size_t)m.triangleCount * 3 > cooked.triangles.size())
			windowsOk = false;
	}
	Check(windowsOk, "meshlet.quad.windows");
	// Indirect staging expands to the same triangle soup.
	std::vector<uint32_t> idx;
	std::vector<smvk::MeshletDrawCommand> cmds;
	smvk::BuildMeshletIndirectCommands(cooked, idx, cmds);
	Check(cmds.size() == cooked.meshlets.size() && idx.size() == 6, "meshlet.indirect.count");
	// Disabled config fails softly with empty output.
	smvk::MeshletConfig off;
	smvk::CookedMeshlets empty;
	Check(!smvk::BuildMeshletsForUpload(quad, off, &empty, nullptr, nullptr), "meshlet.disabled.false");
	Check(empty.empty(), "meshlet.disabled.empty");
	// Degenerate input fails softly.
	smvk::StaticMeshUpload deg;
	Check(!smvk::BuildMeshletsForUpload(deg, cfg, &empty, nullptr, nullptr), "meshlet.empty.false");

	// Containment invariant: every cooked bound sphere must contain its
	// meshlet's triangles (the HZB occlusion + cone tests rely on it; a
	// violation false-culls visible meshlets). Grid + one long sliver
	// forces disparate verts into shared meshlets (cf. sponza prim.49).
	{
		smvk::StaticMeshUpload u;
		const int N = 8;
		for (int iy = 0; iy <= N; ++iy)
			for (int ix = 0; ix <= N; ++ix) {
				smvk::StaticVertex v{};
				v.pos = { (float)ix, (float)iy, 0.0f };
				u.vertices.push_back(v);
			}
		auto at = [N](int ix, int iy) { return (uint32_t)(iy * (N + 1) + ix); };
		for (int iy = 0; iy < N; ++iy)
			for (int ix = 0; ix < N; ++ix) {
				u.indices.insert(u.indices.end(), { at(ix, iy), at(ix + 1, iy), at(ix, iy + 1) });
				u.indices.insert(u.indices.end(), { at(ix + 1, iy), at(ix + 1, iy + 1), at(ix, iy + 1) });
			}
		// Long sliver: two verts in the grid, one 60 units away.
		uint32_t s0 = (uint32_t)u.vertices.size();
		smvk::StaticVertex a{}; a.pos = { 0.0f, 0.0f, 0.0f };
		smvk::StaticVertex b{}; b.pos = { 0.5f, 0.0f, 0.0f };
		smvk::StaticVertex c{}; c.pos = { 60.0f, 0.5f, 0.0f };
		u.vertices.push_back(a); u.vertices.push_back(b); u.vertices.push_back(c);
		u.indices.insert(u.indices.end(), { s0, s0 + 1, s0 + 2 });
		smvk::MeshletConfig sc = cfg;
		sc.maxVertices = 64;
		sc.maxTriangles = 32;
		smvk::CookedMeshlets sliv;
		Check(smvk::BuildMeshletsForUpload(u, sc, &sliv, nullptr, nullptr), "meshlet.sliver.ok");
		bool contained = !sliv.empty();
		for (const auto& m : sliv.meshlets) {
			for (uint32_t t = 0; t < m.triangleCount && contained; ++t) {
				for (uint32_t k = 0; k < 3 && contained; ++k) {
					uint32_t local = sliv.triangles[m.triangleOffset + t * 3 + k];
					if (local >= m.vertexCount) { contained = false; break; }
					uint32_t vid = sliv.vertices[m.vertexOffset + local];
					if (vid >= u.vertices.size()) { contained = false; break; }
					const auto& p = u.vertices[vid].pos;
					float dx = p.x - m.boundCenter.x;
					float dy = p.y - m.boundCenter.y;
					float dz = p.z - m.boundCenter.z;
					float d = std::sqrt(dx * dx + dy * dy + dz * dz);
					if (d > m.boundRadius + 1e-3f * std::max(1.0f, m.boundRadius))
						contained = false;
				}
			}
		}
		Check(contained, "meshlet.sliver.contained");
	}
}

void TestStaticCollapse() {
	using namespace smvk;
	auto cluster = std::make_unique<Cluster>(nullptr);
	cluster->SetCookMode(true);

	// Two quads sharing one material; second translated +1 on X.
	auto q1 = MakeQuad();
	auto q2 = MakeQuad();
	MaterialCreateInfo ci{};
	const ClusterMaterialHandle math = cluster->CreateMaterial(ci);
	const uint32_t matIdx = math.index();

	const int root = addNode(cluster->scene, -1, 0);
	const int n1 = addNode(cluster->scene, root, 1);
	const int n2 = addNode(cluster->scene, root, 1);
	const ClusterStaticMeshHandle h1 = cluster->UploadStaticMesh(q1);
	const ClusterStaticMeshHandle h2 = cluster->UploadStaticMesh(q2);
	cluster->scene.meshForNode[n1] = h1.index();
	cluster->scene.meshForNode[n2] = h2.index();
	cluster->scene.materialForNode[n1] = matIdx;
	cluster->scene.materialForNode[n2] = matIdx;
	cluster->scene.localTransform[n2] = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	MarkSubtreeStatic(cluster->scene, n1);
	MarkSubtreeStatic(cluster->scene, n2);
	markAsChanged(cluster->scene, root);
	recalculateGlobalTransforms(cluster->scene);

	StaticMergeOptions opts;
	opts.deleteSources = true;
	StaticMergeResult mr = CollapseStaticSubtrees(*cluster, opts);
	Check(mr.ok, "collapse.ok");
	Check(mr.srcNodesConsumed == 2, "collapse.consumed");
	Check(mr.blocks.size() == 1, "collapse.singleblock");
	if (!mr.blocks.empty()) {
		const auto h = cluster->staticMeshes.getHandle(mr.blocks[0].meshPoolIndex);
		const StaticMeshUpload* up = cluster->GetRetainedUpload(h);
		Check(up && up->vertices.size() == 8 && up->indices.size() == 12, "collapse.baked.size");
		if (up && up->vertices.size() == 8) {
			// Second quad baked at +1 X: its min.x must be 1, first quad 0.
			float minAll = up->vertices[0].pos.x;
			float maxAll = up->vertices[0].pos.x;
			for (const auto& v : up->vertices) {
				minAll = std::min(minAll, v.pos.x);
				maxAll = std::max(maxAll, v.pos.x);
			}
			Check(minAll == 0.0f && maxAll == 2.0f, "collapse.baked.transform");
		}
		Check(IsStaticNode(cluster->scene, mr.blocks[0].node), "collapse.block.static");
	}
}

} // namespace

int main() {
	TestVfsNormalize();
	TestOverlayMem();
	TestAssetUris();
	TestPakRoundTrip();
	TestPakBackendServesTextures();
	TestMeshOpt();
	TestClusterSerializeCook();
	TestStaticSubtrees();
	TestMeshlets();
	TestStaticCollapse();
	std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
