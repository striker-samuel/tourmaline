// App entry point — the HOST: takes a cooked pak (+ optional scene
// index), opens it, discovers the scenes inside, loads index 0 or the
// requested one into a World (clusters/resources uploaded to the GPU
// context, resolved), and hands it to the app::WorldRenderer
// (App/world_renderer.h), which renders whatever world it is given —
// it knows nothing about paks or scene files.
//
// Usage:
//   App [--pak <path.pak>] [--scene-index N]
//   [--minimal] [--filter-only] [--with <csv>] [--without <csv>]
//   [--bench <frames>] [--bench_move <m/s>] [--present <fifo|mailbox|immediate>]
//   [--disable_control] [--no-dlss] [--use_optick] [--use_optick_gpu]
// --pak runs a cooked pak (default content/paks/sponza.pak); scenes
//   inside are discovered at startup, index 0 loads unless
//   --scene-index picks another. The App loads OUR format only:
//   preprocess glTF through the editor (import -> cook).
// --disable_control freezes the camera (no keyboard/gamepad/mouselook):
//   deterministic A/B benching at the scene's configured pose.
// --bench_move flies the camera forward at a constant velocity (default
//   4 m/s, starts past warmup) so LOD promotion + texture streaming
//   churn through the measured frames — pairs with --disable_control.
// --bench also pins fixed manual exposure (no auto-exposure).
// --no-dlss keeps the Streamline interposer out of the Vulkan chain
//   (RenderDoc captures crash through the SL layer + NGX cubins).
// --use_optick enables the Optick CPU capture session (bench.opt);
// --use_optick_gpu also brings up Optick GPU timelines.

#include <App/world_renderer.h>
#include <Engine/Profile/profile.h>

#include <Engine/Memory/memory.h>

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Flintbase/flintbase_backend.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <Engine/Vfs/vfs.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h> // GetModuleFileNameA (exe-relative pak resolution)
#endif

namespace {

void PrintUsage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "App")
		<< " [--pak <path.pak>] [--scene-index N]\n"
		<< "  [--minimal] [--filter-only] [--with <csv>] [--without <csv>]\n"
		<< "  [--bench <frames>] [--bench_move <m/s>] [--present <fifo|mailbox|immediate>] [--disable_control] [--no-dlss]\n"
		<< "  [--use_optick] [--use_optick_gpu]\n"
		<< "  --pak runs a cooked pak (default content/paks/sponza.pak);\n"
		<< "  scenes inside are discovered at startup, index 0 loads unless\n"
		<< "  --scene-index picks another. The App loads OUR format only:\n"
		<< "  preprocess glTF through the editor (import -> cook).\n"
		<< "  --disable_control freezes camera input for deterministic benching\n"
		<< "  --bench_move flies forward at constant velocity (default 4 m/s) past warmup\n"
		<< "  --bench also pins fixed manual exposure (no auto-exposure)\n"
		<< "  --no-dlss keeps the Streamline interposer out of the Vulkan chain\n"
		<< "  (RenderDoc captures crash through the SL layer + NGX cubins)\n"
		<< "  --use_optick enables the Optick CPU capture session (bench.opt)\n"
		<< "  --use_optick_gpu also brings up Optick GPU timelines (implies --use_optick)\n";
}

// Pak stem -> scene id (GI sidecars resolve gi/<id>.*, check-scene
// extras key on it; see WorldRenderer::SetSceneId).
std::string StemOf(const std::string& p) {
	const size_t slash = p.find_last_of("/\\");
	const std::string file = (slash == std::string::npos)
		? p : p.substr(slash + 1);
	const size_t dot = file.find_last_of('.');
	return (dot == std::string::npos || dot == 0)
		? file : file.substr(0, dot);
}

// Pak path resolution: --pak accepts host paths AND vfs-style paths
// ("/content/..."). Windows reads a leading slash as DRIVE-relative
// ("F:/content/..." — almost never what was meant), so try as-is,
// then CWD-relative, then exe-relative, and open the first that
// exists. Empty when nothing exists.
std::string ResolvePakOpenPath(const std::string& scenePak) {
	std::string pakOpenPath;
	std::vector<std::string> candidates;
	if (!scenePak.empty()) {
		std::string rel = scenePak;
		while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\'))
			rel.erase(rel.begin());
		candidates.push_back(scenePak);
		if (!rel.empty() && rel != scenePak) {
			candidates.push_back(rel);
			char exePath[1024] = {};
#ifdef _WIN32
			DWORD n = GetModuleFileNameA(nullptr, exePath, DWORD(sizeof(exePath)));
			(void)n;
#endif
			if (exePath[0])
				candidates.push_back(
					(std::filesystem::path(exePath).parent_path() / rel).string());
		}
	}
	for (const std::string& c : candidates) {
		std::error_code ec;
		if (!c.empty() && std::filesystem::exists(std::filesystem::path(c), ec)) {
			pakOpenPath = c;
			break;
		}
	}
	if (pakOpenPath.empty() && !scenePak.empty())
		std::cerr << "[App] pak not found: '" << scenePak
			<< "' (tried as-is, CWD-relative and exe-relative)" << std::endl;
	return pakOpenPath;
}

// UV sphere (lat-long) centered on the origin — the sponza check
// scene's chrome probe ball. Triangles follow the (a,b,c),(a,c,d)
// pattern with b one step in +theta (longitude) and d one step in
// +phi (pole to pole): dTheta x dPhi points outward, so the winding
// matches the engine's CCW front faces.
smvk::StaticMeshUpload BuildChromeBallUpload(float radius, int slices, int stacks) {
	smvk::StaticMeshUpload data;
	data.aabbMin = glm::vec3(-radius);
	data.aabbMax = glm::vec3(radius);
	const float pi = glm::pi<float>();
	const int nSlices = std::max(8, slices);
	const int nStacks = std::max(4, stacks);
	for (int j = 0; j <= nStacks; ++j) {
		const float phi = pi * static_cast<float>(j) / static_cast<float>(nStacks);
		const float sinPhi = std::sin(phi);
		const float cosPhi = std::cos(phi);
		for (int i = 0; i <= nSlices; ++i) {
			const float theta = 2.0f * pi * static_cast<float>(i) / static_cast<float>(nSlices);
			const float sinT = std::sin(theta);
			const float cosT = std::cos(theta);
			const glm::vec3 n(sinPhi * cosT, cosPhi, sinPhi * sinT);
			smvk::StaticVertex v{};
			v.pos = n * radius;
			v.color = glm::vec4(1.0f);
			v.texCoord = glm::vec2(
				static_cast<float>(i) / static_cast<float>(nSlices),
				static_cast<float>(j) / static_cast<float>(nStacks));
			v.normal = n;
			glm::vec3 t(-sinT, 0.0f, cosT);
			if (glm::dot(t, t) < 1e-12f)
				t = glm::vec3(1.0f, 0.0f, 0.0f); // poles: theta tangent degenerates
			v.tangent = glm::vec4(glm::normalize(t), 1.0f);
			data.vertices.push_back(v);
		}
	}
	const uint32_t row = static_cast<uint32_t>(nSlices) + 1u;
	for (int j = 0; j < nStacks; ++j) {
		for (int i = 0; i < nSlices; ++i) {
			const uint32_t a = static_cast<uint32_t>(j) * row + static_cast<uint32_t>(i);
			const uint32_t b = a + 1u;
			const uint32_t c = a + row + 1u;
			const uint32_t d = a + row;
			data.indices.push_back(a);
			data.indices.push_back(b);
			data.indices.push_back(c);
			data.indices.push_back(a);
			data.indices.push_back(c);
			data.indices.push_back(d);
		}
	}
	return data;
}

// Chrome probe ball (sponza check scene only): procedural UV sphere,
// 0.5 m in diameter, at the atrium center resting on the floor (y=0).
// White metal (metallic 1, roughness 0; the lit shader clamps
// roughness to 0.04 so there are no GGX singularities). Injected into
// the loaded cluster BEFORE Resolve so it joins culling, shadows, AO
// and reflections like any imported node. Independent root (own
// identity-parent transform), so scene root transforms can't move it
// and the static-freeze loop below simply leaves it dynamic.
void InjectSponzaChromeBall(smvk::World& world, smvk::ResourceManager& ctx) {
	(void)ctx;
	if (world.clusters.empty())
		return;
	smvk::Cluster& sponzaCluster = *world.clusters.back();
	const float ballRadius = 0.25f;
	const glm::vec3 ballCenter(0.0f, ballRadius, 0.0f);
	smvk::StaticMeshUpload ballUpload = BuildChromeBallUpload(ballRadius, 48, 32);
	smvk::ClusterStaticMeshHandle ballMesh = sponzaCluster.UploadStaticMesh(ballUpload);
	smvk::MaterialCreateInfo chromeCi{};
	chromeCi.baseColorFactor = glm::vec4(1.0f);
	chromeCi.metallicFactor = 1.0f;
	chromeCi.roughnessFactor = .13f;
	chromeCi.emissiveFactor = glm::vec3(0.0f);
	chromeCi.alphaMode = smvk::AlphaMode::Opaque;
	chromeCi.shadingModel = smvk::ShadingModel::DefaultLit;
	smvk::ClusterMaterialHandle chromeMat = sponzaCluster.CreateMaterial(chromeCi);
	if (ballMesh.empty() || chromeMat.empty()) {
		std::cerr << "[Scene] chrome ball inject failed (mesh/material)\n";
		return;
	}
	const int ballNode = addNode(sponzaCluster.scene, -1, 0);
	sponzaCluster.scene.localTransform[ballNode] =
		glm::translate(glm::mat4(1.0f), ballCenter);
	sponzaCluster.scene.globalTransform[ballNode] =
		glm::translate(glm::mat4(1.0f), ballCenter);
	sponzaCluster.scene.meshForNode[ballNode] = ballMesh.index();
	sponzaCluster.scene.materialForNode[ballNode] = chromeMat.index();
	setNodeName(sponzaCluster.scene, ballNode, "chrome_ball");
	markAsChanged(sponzaCluster.scene, ballNode);
	std::cerr << "[Scene] chrome ball: r=" << ballRadius
		<< "m at (" << ballCenter.x << "," << ballCenter.y << "," << ballCenter.z << "), "
		<< ballUpload.vertices.size() << " verts, "
		<< ballUpload.indices.size() / 3 << " tris\n";
}

// Background pak preload result. KickPakPreload (below) runs in main()
// BEFORE the renderer starts Vulkan, so the file read + whole-image CRC +
// per-entry LZ4 decompression overlap instance/device/swapchain init and
// shader loading. The backend owns the image + the parsed reader (already
// decompressed); the world source consumes it on the render thread once
// the context exists. `backend` is null on any failure (error filled).
struct PakPreload {
	std::string scenePak; // display name (also the VFS mount name)
	std::string openPath; // resolved host path (empty = not found)
	std::shared_ptr<const std::vector<uint8_t>> image;
	std::shared_ptr<smvk::PakBackend> backend;
	std::string error;
};

std::future<PakPreload> KickPakPreload(const std::string& pakPath) {
	return std::async(std::launch::async, [pakPath]() {
		const auto t0 = std::chrono::steady_clock::now();
		PakPreload p;
		p.scenePak = smvk::FlintbaseHostPath(pakPath);
		p.openPath = ResolvePakOpenPath(p.scenePak);
		if (p.openPath.empty()) {
			p.error = "pak not found: '" + p.scenePak
				+ "' (tried as-is, CWD-relative and exe-relative)";
			return p;
		}
		std::ifstream pakFile(p.openPath, std::ios::binary | std::ios::ate);
		if (!pakFile) {
			p.error = "pak open failed: '" + p.openPath + "'";
			return p;
		}
		const auto pakEnd = pakFile.tellg();
		if (pakEnd == std::ifstream::pos_type(-1)) {
			p.error = "pak unreadable: '" + p.openPath + "'";
			return p;
		}
		pakFile.seekg(0);
		auto image = std::make_shared<std::vector<uint8_t>>(
			static_cast<size_t>(pakEnd));
		if (!pakFile.read(reinterpret_cast<char*>(image->data()),
				static_cast<std::streamsize>(image->size()))) {
			p.error = "pak read failed: '" + p.openPath + "'";
			return p;
		}
		p.image = std::move(image);
		const auto t1 = std::chrono::steady_clock::now();
		// ONE open covers everything: PakBackend::Open parses + verifies
		// the CRC + (for big images) parallel-decompresses every LZ4
		// entry. Its Reader() serves scene discovery + the cluster load
		// below; the old path opened the reader twice (double CRC +
		// double decompress).
		std::string pakErr;
		p.backend = smvk::PakBackend::Open(p.image, p.scenePak, pakErr);
		const auto t2 = std::chrono::steady_clock::now();
		if (!p.backend) {
			p.image.reset();
			p.error = "pak parse failed: " + pakErr;
			return p;
		}
		std::cerr << "[App] pak preloaded (read "
			<< std::chrono::duration<double>(t1 - t0).count() << "s, open "
			<< std::chrono::duration<double>(t2 - t1).count() << "s): "
			<< p.backend->Reader().EntryCount() << " entries, "
			<< p.image->size() / (1024 * 1024) << " MB\n";
		return p;
	});
}

// The HOST's pak loader (the renderer's world source): consumes the
// preloaded backend (read + parse + decompress already overlapped with
// renderer init), loads the selected scene into a World (uploads issued
// to the context — everything renders by handles), injects the
// check-scene extras, freezes the cooked roots static and resolves.
// The pak's embedded textures mount into the renderer's content
// overlay (below loose content/, so dev overrides win) for init-time
// reads (cubemaps baked into the pak, ...). Returns nullptr on any
// failure (the renderer fails loudly).
smvk::mem::LinearPtr<smvk::World> LoadPakWorld(PakPreload pre,
	int sceneIndex, smvk::ResourceManager& ctx, smvk::OverlayVfs& contentVfs,
	smvk::TextureStreamer* sceneStreamer) {
	if (!pre.backend) {
		std::cerr << "[App] pak preload failed: " << pre.error << "\n";
		return nullptr;
	}
	std::cerr << "[App] loading pak '" << pre.scenePak << "'\n";
	const smvk::PakReader& reader = pre.backend->Reader();
	std::cerr << "[App] pak parsed (" << reader.EntryCount() << " entries)\n";
	// Scene discovery: list, default to index 0.
	std::string lerr;
	auto scenes = smvk::ListPakScenes(reader, &lerr);
	if (scenes.empty()) {
		std::cerr << "[App] no loadable scenes: " << lerr << "\n";
		return nullptr;
	}
	for (size_t i = 0; i < scenes.size(); ++i)
		std::cerr << "[App]   scene[" << i << "] '" << scenes[i].name
			<< "' nodes=" << scenes[i].nodes << "\n";
	if (sceneIndex < 0 || size_t(sceneIndex) >= scenes.size()) {
		std::cerr << "[App] scene index " << sceneIndex << " out of range ("
			<< scenes.size() << " scenes)\n";
		return nullptr;
	}
	std::cerr << "[App] loading scene[" << sceneIndex << "] '"
		<< scenes[size_t(sceneIndex)].name << "'...\n";
	auto world = smvk::mem::MakeProcess<smvk::World>();
	try {
		world->LoadClusterFromPak(ctx, reader, uint32_t(sceneIndex),
			sceneStreamer, pre.image);
	}
	catch (const std::exception& e) {
		// DeserializeCluster drains its own staged uploads on failure;
		// drain anything queued since, then fail the load.
		std::cerr << "[App] pak load failed: " << e.what() << "\n";
		ctx.WaitForUploads();
		return nullptr;
	}
	std::cerr << "[App] pak cluster loaded\n";
	// Check-scene extras (sponza chrome ball) inject BEFORE resolve so
	// they join culling/shadows/AO/reflections like any imported node.
	if (StemOf(pre.scenePak) == "sponza")
		InjectSponzaChromeBall(*world, ctx);
	// Freeze everything static (no recook needed): the exporters stamp
	// no STATIC_ marks, so cooked paks carry staticRoots=0 and the
	// World's static BVH would stay empty. These scenes never animate,
	// so mark each cluster root static here; Resolve carries the spec
	// into the world scene and RebuildStaticBvh picks it up. Future
	// dynamic nodes must ClearSubtreeStatic() before Resolve to opt out.
	for (auto& clusterPtr : world->clusters) {
		if (clusterPtr && !clusterPtr->scene.hierarchy.empty())
			MarkSubtreeStatic(clusterPtr->scene, 0);
	}
	world->ResolveAndFixup_clusters();
	// Serve pak-embedded textures through the renderer's content
	// overlay (below loose content/, so dev overrides win).
	contentVfs.Mount(pre.backend, 10, pre.scenePak);
	return world;
}

} // namespace

  	int main(int argc, const char* argv[]) {
	// Reserve linear-allocator virtual address space FIRST, before worker
	// threads, Vulkan, or asset loading (see Engine/Memory/memory.h). An
	// unsatisfiable reservation fails fast here with the OOM diagnostic.
	smvk::mem::InitMemory();
	std::string pakPath = "content/paks/sponza.pak";
	int sceneIndex = 0;
	bool minimal = false;
	bool filterOnly = false;
	const char* withCsv = nullptr;
	const char* withoutCsv = nullptr;
	int benchFrames = 0;
	float benchMoveSpeed = 0.0f;
	const char* presentMode = nullptr;
	bool disableControl = false;
	bool useOptick = false;
	bool useOptickGpu = false;
	bool noDlss = false;
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if ((std::strcmp(a, "--pak") == 0) && i + 1 < argc) {
			pakPath = argv[++i];
		}
		else if (std::strncmp(a, "--pak=", 6) == 0) {
			pakPath = a + 6;
		}
		else if (std::strcmp(a, "--gltf") == 0
			|| std::strncmp(a, "--gltf=", 7) == 0) {
			std::cerr << "--gltf was removed: the App loads OUR format "
				"only (cooked paks). Preprocess glTF through the "
				"editor (import -> cook), then --pak the result.\n";
			return EXIT_FAILURE;
		}
		else if ((std::strcmp(a, "--scene-index") == 0) && i + 1 < argc) {
			sceneIndex = std::atoi(argv[++i]);
		}
		else if (std::strncmp(a, "--scene-index=", 14) == 0) {
			sceneIndex = std::atoi(a + 14);
		}
		else if (std::strcmp(a, "--minimal") == 0) {
			minimal = true;
		}
		else if (std::strcmp(a, "--filter-only") == 0) {
			filterOnly = true;
		}
		else if ((std::strcmp(a, "--with") == 0) && i + 1 < argc) {
			withCsv = argv[++i];
		}
		else if (std::strncmp(a, "--with=", 7) == 0) {
			withCsv = a + 7;
		}
		else if ((std::strcmp(a, "--without") == 0) && i + 1 < argc) {
			withoutCsv = argv[++i];
		}
		else if (std::strncmp(a, "--without=", 10) == 0) {
			withoutCsv = a + 10;
		}
		else if (std::strcmp(a, "--bench") == 0 && i + 1 < argc) {
			benchFrames = std::atoi(argv[++i]);
		}
		else if (std::strncmp(a, "--bench=", 8) == 0) {
			benchFrames = std::atoi(a + 8);
		}
		else if (std::strcmp(a, "--bench_move") == 0
			|| std::strcmp(a, "--bench-move") == 0) {
			// Bare flag: default flight speed (4 m/s walks the bistro street
			// at a good LOD/streaming churn rate). Optional value follows.
			benchMoveSpeed = 4.0f;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				benchMoveSpeed = static_cast<float>(std::atof(argv[++i]));
		}
		else if (std::strncmp(a, "--bench_move=", 14) == 0
			|| std::strncmp(a, "--bench-move=", 13) == 0) {
			const char* v = a + (std::strncmp(a, "--bench_move=", 14) == 0 ? 14 : 13);
			benchMoveSpeed = static_cast<float>(std::atof(v));
		}
		else if ((std::strcmp(a, "--present") == 0) && i + 1 < argc) {
			presentMode = argv[++i];
		}
		else if (std::strncmp(a, "--present=", 10) == 0) {
			presentMode = a + 10;
		}
		else if (std::strcmp(a, "--disable_control") == 0 || std::strcmp(a, "--disable-control") == 0) {
			disableControl = true;
		}
		else if (std::strcmp(a, "--use_optick") == 0) {
			useOptick = true;
		}
		else if (std::strcmp(a, "--use_optick_gpu") == 0) {
			useOptick = true;
			useOptickGpu = true;
		}
		else if (std::strcmp(a, "--no-dlss") == 0 || std::strcmp(a, "--no_dlss") == 0) {
			noDlss = true;
		}
		else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
			PrintUsage(argv[0]);
			return EXIT_SUCCESS;
		}
		else {
			std::cerr << "Unknown argument: " << a << "\n";
			PrintUsage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	// Entrypoint: a pak file + a scene index (0 = default). The scene
	// id (GI sidecar stem + check-scene gating) is the pak stem.
	std::string sceneId = StemOf(pakPath);
	if (sceneId.empty())
		sceneId = "pak";
	std::cerr << "Scene: pak '" << pakPath << "' (id '" << sceneId
		<< "', index " << sceneIndex << ")\n";

	// Pak preload: kick the file read + parse + decompress NOW, before
	// the renderer starts, so it overlaps Vulkan init + shader loading
	// (the old flow did all of it inside Initialize, after the GPU and
	// every shader were already up). The future rides a shared_ptr so
	// the copyable WorldSource can hold it until the renderer calls.
	auto preloadFuture = std::make_shared<std::future<PakPreload>>(
		KickPakPreload(pakPath));

	app::WorldRenderer renderer;
	renderer.SetSceneId(sceneId);
	// The HOST owns scene loading: the renderer calls this once during
	// Initialize, after its context exists (nothing can upload before
	// that), and renders the returned world by handles. By then the
	// preload has typically been running for the whole of renderer init.
	// The renderer's texture streamer rides along so v4 digest textures
	// register for demand upgrades (null when streaming is off → full
	// resident, the pre-v4 behavior). The preloaded image pins the
	// borrowed tail payloads.
	renderer.SetWorldSource(
		[preloadFuture, sceneIndex, &renderer](smvk::ResourceManager& ctx,
			smvk::OverlayVfs& contentVfs) -> smvk::mem::LinearPtr<smvk::World> {
			return LoadPakWorld(preloadFuture->get(), sceneIndex,
				ctx, contentVfs, renderer.GetTextureStreamer());
		});
	app::ScenePassConfig passCfg = app::ScenePassConfig::All();
	if (filterOnly)
		passCfg = app::ScenePassConfig::FilterOnly();
	else if (minimal)
		passCfg = app::ScenePassConfig::Minimal();
	// Bisection: layer extra passes onto the base config
	// (csv: deferred,shadows,ssao/gtao/ao,smaa,motion,dof,ssr,bloom,exposure,msaa,skydome,ui,cull,particles).
	auto applyCsv = [&](const char* csv, bool enable) {
		if (!csv) return;
		const std::string s = csv;
		size_t pos = 0;
		while (pos <= s.size()) {
			size_t end = s.find(',', pos);
			if (end == std::string::npos) end = s.size();
			const std::string tok = s.substr(pos, end - pos);
			bool* flag = nullptr;
			if (tok == "deferred") flag = &passCfg.deferredShadows;
			else if (tok == "shadows") { passCfg.directionalShadows = passCfg.spotShadows = enable; }
			else if (tok == "ssao" || tok == "gtao" || tok == "ao") flag = &passCfg.ssao;
			else if (tok == "smaa") flag = &passCfg.smaa;
			else if (tok == "motion") { passCfg.motionVectors = passCfg.motionBlur = enable; }
			else if (tok == "dof") flag = &passCfg.dof;
			else if (tok == "ssr") flag = &passCfg.ssr;
			else if (tok == "bloom") flag = &passCfg.bloom;
			else if (tok == "exposure") flag = &passCfg.exposure;
			else if (tok == "msaa") flag = &passCfg.msaa;
			else if (tok == "skydome") flag = &passCfg.skydome;
			else if (tok == "ui") flag = &passCfg.ui;
			else if (tok == "cull") flag = &passCfg.forwardPlusCull;
			else if (tok == "prepass") flag = &passCfg.forwardPlusPrepass;
			else if (tok == "filter") flag = &passCfg.gfxFilter;
			else if (tok == "particles") flag = &passCfg.particles;
			else if (!tok.empty()) std::cerr << "Unknown pass token: '" << tok << "'\n";
			if (flag) *flag = enable;
			pos = end + 1;
		}
	};
	applyCsv(withCsv, true);
	applyCsv(withoutCsv, false);
	renderer.Configure(passCfg);
	// --no-dlss: suppress the Streamline load BEFORE the renderer runs
	// (smvk::App::Initialize consults the flag before DlssStartup /
	// pointing volk at the interposer — after that the decision is locked
	// in) and force the mode off so no feature evaluation happens either.
	// This is the RenderDoc-safe configuration.
	if (noDlss) {
		smvk::DlssSetStartupEnabled(false);
		renderer.SetDlssMode(smvk::DlssMode::Off);
		std::cerr << "[DLSS] disabled (--no-dlss): Streamline interposer not loaded\n";
	}
	if (disableControl) {
		renderer.SetDisableControl(true);
		std::cerr << "[Scene] camera control disabled (--disable_control)\n";
	}
	if (presentMode) {
		if (std::strcmp(presentMode, "fifo") == 0)
			renderer.SetPresentMode(smvk::PresentMode::Fifo);
		else if (std::strcmp(presentMode, "mailbox") == 0)
			renderer.SetPresentMode(smvk::PresentMode::Mailbox);
		else if (std::strcmp(presentMode, "immediate") == 0)
			renderer.SetPresentMode(smvk::PresentMode::Immediate);
		else {
			std::cerr << "Unknown present mode: '" << presentMode << "' (fifo|mailbox|immediate)\n";
			return EXIT_FAILURE;
		}
	}
	if (benchFrames > 0) {
		renderer.SetFrameBudget(benchFrames);
		// Bench captures must be pixel-comparable across runs: drop eye
		// adaptation and pin the fixed manual exposure (X re-enables live).
		renderer.SetAutoExposureEnabled(false);
		std::cout << "bench: auto-exposure off (fixed manual exposure)\n";
	}
	if (benchMoveSpeed > 0.0f) {
		// Constant-velocity flight needs --bench to be meaningful (the
		// movement gates on the measured window); pair them up here so a
		// bare --bench_move still does the right thing.
		if (benchFrames <= 0) {
			benchFrames = 300;
			renderer.SetFrameBudget(benchFrames);
			renderer.SetAutoExposureEnabled(false);
			std::cout << "bench: auto-exposure off (fixed manual exposure)\n";
		}
		renderer.SetBenchMoveSpeed(benchMoveSpeed);
		std::cout << "bench: constant-velocity flight " << benchMoveSpeed
			<< " m/s forward from the spawn pose (past warmup)\n";
	}
	// Optick gates must be set before Run(): the engine initializes the GPU
	// profiler during VulkanCore init, and the bench session starts after the
	// warmup frames. --use_optick_gpu implies --use_optick.
	smvk::profiler::SetEnabled(useOptick);
	if (useOptickGpu)
		smvk::profiler::SetGpuEnabled(true);
	try {
		renderer.Run();
	}
	catch (const std::exception& e) {
		std::cerr << "\nERROR: " << e.what() << std::endl;
		std::cerr << "\nPress Enter to exit...";
		std::cin.get();
		return EXIT_FAILURE;
	}
	smvk::mem::ShutdownMemory(); // footprint report (used/committed/reserved)
	return EXIT_SUCCESS;
}
