#pragma once
// SceneRenderer: whole-frame renderer class extracted from the former
// app/main.cpp TestApp. It owns every framegraph resource (pipelines,
// targets, per-frame buffers) plus the scene (world cluster) and appends
// all passes every frame from a configurable pass set, so adding a scene
// sample (Sponza, Bistro, ...) is a SceneDesc row — never a main.cpp copy.
//
// Typical use (see app/src/main.cpp):
//   app::SceneRenderer renderer;
//   renderer.Configure(app::SceneRegistry::Find("bistro").value(),
//                      app::ScenePassConfig::All());
//   renderer.Run(); // smvk::AppImpl main loop

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/app.h>
#include <Engine/App/world.h>
#include <Engine/Camera/camera.h>
#include <Engine/ImGui/imgui_overlay.h>
#include <Engine/Renderer/cookies.h>
#include <Engine/Renderer/renderview.h>
#include <Engine/Rendergraph/bloom.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Rendergraph/environment.h>
#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/Rendergraph/hdr.h>
#include <Engine/Rendergraph/motionblur.h>
#include <Engine/Rendergraph/motionvector.h>
#include <Engine/Rendergraph/msaa.h>
#include <Engine/Rendergraph/spotshadow.h>
#include <Engine/Rendergraph/ssao.h>
#include <Engine/Rendergraph/tonemap.h>
#include <Engine/Rendergraph/ui.h>

#include <App/forwardplus_renderer.h>
#include <App/frame_context.h>
#include <App/push_constants.h>
#include <App/renderer.h>

namespace app {

// ---------------------------------------------------------------------------
// Per-scene sample descriptor (replaces the former file-static
// gltf_meshes_paths / pak_path globals). A new sample = one row in
// SceneRegistry::All(); the framegraph is shared.
// ---------------------------------------------------------------------------
struct SceneDesc {
	const char* id = "sponza";
	const char* label = "Sponza";
	const char* gltfPath = "content/models/Sponza/sponza.gltf";
	const char* pakPath = "content/paks/sponza.pak";
	glm::vec3 camPos{ 0.0f, 1.0f, 0.0f };
	glm::vec3 camTarget{ 0.0f, 1.0f, 1.0f };
	float farClip = 50.0f;
	// Coordinate system fix for glTF import:
	// 0 = engine fix (180° Y: -X, +Y, -Z), 1 = skip engine fix, 2 = invert Y only,
	// 3 = cancel root -90° X rotation + 100x scale (for Niagara bistro)
	int coordFix = 0;
	// Optional pre-transform applied to scene root after loading (post coordFix)
	glm::mat4 preTransform{ 1.0f };
};

struct SceneRegistry {
	static const SceneDesc& Sponza();
	static const SceneDesc& Bistro();
	static const SceneDesc& BistroNiagara();
	static std::vector<SceneDesc> All();
	// Empty optional when the id is unknown (caller prints usage).
	static std::optional<SceneDesc> Find(const std::string& id);
};

// ---------------------------------------------------------------------------
// Configurable pass set. RenderFrame consults these flags, so scene samples
// (or A/B experiments) toggle passes without touching pass code. Defaults
// are the full pipeline (previous behavior).
// ---------------------------------------------------------------------------
struct ScenePassConfig {
	bool directionalShadows = true; // CSM cascade passes
	bool spotShadows = true;        // spot atlas selection + tile passes
	bool forwardPlusPrepass = true; // depth prepass feeding the tile cull
	bool forwardPlusCull = true;    // tile light cull
	bool gfxFilter = true;          // GPU-driven per-view triangle filter
	bool filteredMotion = true;     // filtered motion-vector path
	bool motionVectors = true;      // motion-vector target pass
	bool motionBlur = true;         // HDR velocity-gather blur
	bool ssao = true;               // half-res depth AO
	bool bloom = true;              // HDR bloom chain
	bool tonemap = true;            // HDR -> swapchain tonemap
	bool ui = true;                 // separated ImGui target + composite
	bool msaa = true;               // 2x MSAA Main (when device supports it)
	bool skydome = true;            // fullscreen-triangle sky in Main

	static ScenePassConfig All() { return ScenePassConfig{}; }
	static ScenePassConfig Minimal() {
		ScenePassConfig c{};
		c.directionalShadows = false;
		c.spotShadows = false;
		c.forwardPlusPrepass = false;
		c.forwardPlusCull = false;
		c.gfxFilter = false;
		c.filteredMotion = false;
		c.motionVectors = false;
		c.motionBlur = false;
		c.ssao = false;
		c.bloom = false;
		c.ui = false;
		c.msaa = false;
		c.skydome = false;
		return c;
	}
};

// Procedural demo lights (grid of points + a few orbiting spots).
struct ForwardPlusDemo {
	bool enabled = true;
	int gridX = 8;
	int gridY = 3;
	int gridZ = 8;
	float spacingX = 3.0f;
	float spacingY = 2.2f;
	float spacingZ = 1.8f;
	float baseY = 1.2f;
	float range = 9.0f;
	float intensity = 600.0f;
	bool animate = true;
	float speed = 0.6f;
	int spotCount = 4;
	float spotRange = 100.0f;
	float spotIntensity = 900.0f;
	// Spot shadows: per-light opt-in flag (demo sets it on all spots) +
	// frame upper limit + screenspace/distance culling. Points never shadow.
	bool spotShadowsEnabled = true;
	int spotShadowMax = 4;               // upper limit, clamped to kMaxSpotShadows
	float spotShadowMaxDistance = 45.0f; // hard cutoff on camera->light distance (m)
	// Light cookies: per-spot gobo projected through the cone; spot s gets
	// pattern s % kCookiePatternCount (soft disc / blinds / starburst).
	bool cookiesEnabled = true;
	// Light functions: procedural per-light intensity curve on the spots
	// (flicker / pulse / cone bands), animated with frame time.
	bool lightFunctionsEnabled = true;
	int functionType = smvk::kForwardPlusFunction_Flicker;
	float functionSpeed = 0.6f;
	float functionScale = 0.65f;
	// Emissive ball objects for the point lights (see PointLightBall): the
	// point lights are real nodes in the scene graph, each carrying a small
	// unlit emissive sphere. Balls never cast shadows (flag cleared).
	float ballRadius = 0.12f;     // world-space radius of each ball (m)
	float ballEmissive = 1200.0f; // HDR emissive strength (bloom threshold ~1000)
};

// One scene-graph node per demo point light: an emissive ball mesh with an
// unlit material. The node's global transform is the light position (the
// Forward+ light buffer is built from it, see UpdateForwardPlusLights);
// the ball itself renders as pure emissive (no lighting, no shadows).
struct PointLightBall {
	NodeId node = UINT32_MAX;
	bool active = false;
};

struct FrameUniforms
{
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewProj;

	glm::mat4 invView;
	glm::mat4 invProj;
	glm::mat4 invViewProj;
	glm::mat4 prevViewProj; // last frame's proj*view (post Y-flip) – motion vectors

	glm::vec3 cameraPos;
	float     time;

	glm::vec2 viewportSize;
	glm::vec2 invViewportSize;

	float nearPlane;
	float farPlane;
	float exposure;
	// SSAO consumption (lighting-pass ambient): full-res composed AO
	// (upscaled by the SsaoCompose pass with depth authority, see ao.hlsli).
	// Must match HLSL FrameUniforms in common.hlsli — offsets asserted below.
	uint32_t aoTextureIndex = UINT32_MAX; // full-res composed AO bindless
	float aoIntensity = 0.0f;            // ambient composite strength
	// Explicit pad so env lands at offset 512 (16-byte aligned), matching
	// HLSL FrameUniforms::aoPad. Without this the HLSL compiler packs env
	// at offset 500, which is both a spirv-val Block-layout violation for
	// PhysicalStorageBuffer and a CPU/GPU desync (garbage sun/IBL/fog).
	float aoPad_[3] = {};
	// Configurable skydome IBL + fog (matches HLSL FrameUniforms.env).
	smvk::EnvironmentFrameData env;
};
static_assert(sizeof(FrameUniforms) % 16 == 0);
// CPU/HLSL ABI: total-size alignment is not enough — each field offset must
// match common.hlsli FrameUniforms (float4-packed, env = 8 x float4).
static_assert(sizeof(FrameUniforms) == 7 * 64 + 16 + 16 + 32 + 128);
static_assert(offsetof(FrameUniforms, view) == 0);
static_assert(offsetof(FrameUniforms, proj) == 64);
static_assert(offsetof(FrameUniforms, viewProj) == 128);
static_assert(offsetof(FrameUniforms, invView) == 192);
static_assert(offsetof(FrameUniforms, invProj) == 256);
static_assert(offsetof(FrameUniforms, invViewProj) == 320);
static_assert(offsetof(FrameUniforms, prevViewProj) == 384);
static_assert(offsetof(FrameUniforms, cameraPos) == 448);
static_assert(offsetof(FrameUniforms, time) == 460);
static_assert(offsetof(FrameUniforms, viewportSize) == 464);
static_assert(offsetof(FrameUniforms, invViewportSize) == 472);
static_assert(offsetof(FrameUniforms, nearPlane) == 480);
static_assert(offsetof(FrameUniforms, farPlane) == 484);
static_assert(offsetof(FrameUniforms, exposure) == 488);
static_assert(offsetof(FrameUniforms, aoTextureIndex) == 492);
static_assert(offsetof(FrameUniforms, aoIntensity) == 496);
static_assert(offsetof(FrameUniforms, env) == 512);

// Resources
struct AppResources {
	smvk::Holder<smvk::BufferHandle> frameUniforms;
	smvk::Holder<smvk::BufferHandle> lightsBuffer;
	smvk::Holder<smvk::BufferHandle> materialsBuffer;
	smvk::Holder<smvk::BufferHandle> directionalCascadeBuffer; // GpuDirectionalLightDescription per frame
	smvk::Holder<smvk::BufferHandle> fpLightsBuffer; // ForwardPlusLight * FP_MAX_LIGHTS per frame
	smvk::LogicalBufferHandle fpLightsLogical{}; // rendergraph alias (stable: buffer never resized)
	smvk::Holder<smvk::BufferHandle> spotShadowBuffer; // GpuCascadeDescription * kMaxSpotShadows per frame
	smvk::LogicalBufferHandle spotShadowLogical{}; // rendergraph alias (stable: buffer never resized)
	smvk::Holder<smvk::ShaderModuleHandle> vertShader;
	smvk::Holder<smvk::ShaderModuleHandle> fragShader;
	smvk::ShaderProgram mainProgram;
	// Alpha-tested shadow-depth program for MASK cutout casters
	// (shadow.vert mainVS + shadow.frag mainPS). Opaque casters keep
	// the engine builder's depth-only program.
	smvk::Holder<smvk::ShaderModuleHandle> shadowMaskedVertShader;
	smvk::Holder<smvk::ShaderModuleHandle> shadowMaskedFragShader;
	smvk::ShaderProgram shadowMaskedProgram;
	// Alpha-tested depth-prepass program for MASK cutout
	// (default.vert mainVS + shadow.frag mainPS): writes cutout-accurate
	// depth so background behind foliage holes survives the main pass.
	// Reuses the main VS and masked-shadow FS modules (no new shaders).
	smvk::ShaderProgram prepassMaskedProgram;
	// NOTE: shadow/bloom/tonemap/forwardplus programs + modules are owned by
	// their engine builders (CascadedShadowAtlas / BloomChain / Tonemapper /
	// ForwardPlus).
};

struct ViewportResources {
	smvk::CascadedShadowAtlas directionalShadows; // atlas + default shadow program + Ensure
	smvk::SpotShadowAtlas spotShadows; // perspective atlas for shadowed spots + Ensure
	smvk::BloomChain bloom; // separated per-mip HDR chain + default down/up programs + Ensure
	smvk::Tonemapper tonemap; // default tonemap program + pass helper
	smvk::ForwardPlus forwardPlus; // tile buffers + cull/depth programs + Ensure
	std::vector<smvk::LogicalImageViewHandle> swapImages;
	smvk::DepthTarget renderDepth; // scene depth at internal resolution + Ensure
	smvk::HdrTarget hdr; // HDR intermediate (image + view + bindless + logical) + Ensure
	smvk::MsaaTarget msaa; // transient 2x color+depth for Main, resolves to hdr
	smvk::MotionVectors motionVectors; // RG16F motion target + program + pass helper
	smvk::MotionBlur motionBlur; // blurred HDR output + velocity-gather program + pass
	smvk::Ssao ssao; // half-res raw + blurred AO targets + programs + passes
	smvk::UiTarget ui; // separated UI pass target (ui scale) + Ensure
	smvk::UiComposite uiComposite; // UI -> swapchain alpha composite program + pass
	bool msaaEnabled = true; // runtime toggle (effective only if device supports)
};

class SceneRenderer : public smvk::AppImpl {
	using Super = smvk::AppImpl;
public:
	void Configure(SceneDesc scene, ScenePassConfig passes);
	const SceneDesc& GetScene() const { return m_scene; }
	ScenePassConfig& Passes() { return m_passes; }
	// Debug: skip the cooked pak and import the scene straight from the
	// SceneDesc glTF path (loose content). A/B against the pak to tell
	// pak/meshlet bugs apart from import/draw-path bugs.
	void SetForceGltf(bool v) { m_forceGltf = v; }

protected:
	void UpdateCamera(float dt);
	void SetUIMode(bool ui);
	// Keeps the projection aspect in sync with the swapchain. Must run
	// whenever the swapchain extent changes (init + resize); otherwise the
	// image stretches and projection-derived data (CSM splits/frusta,
	// selection) uses a stale aspect.
	// NOTE: Camera::setPerspective forwards to MakeInfReversedZProjRH, whose
	// fovY is in radians and whose far plane is infinite (zFar is ignored,
	// near maps to depth 1).
	void UpdateCameraAspect() {
		const auto ext = m_swapchain->GetExtent();
		if (ext.width == 0 || ext.height == 0)
			return;
		m_camera.setPerspective(
			glm::radians(kCameraFovYDegrees),
			float(ext.width) / float(ext.height),
			default_near_clip, m_scene.farClip
		);
	}
	bool m_uiMode = false;
	// Configurable resolution knobs (see "Resolution config" in the .cpp).
	float m_renderScale = 1.0f;
	float m_uiScale = 1.0f;
	float m_uiDpiScale = 1.0f; // synced from ImGuiOverlay at init (auto = monitor DPI)

	void EarlyUpdate() override {
		Super::EarlyUpdate();
	}

	void Update() {
		Super::Update();
		if (m_input.isKeyPressed(GLFW_KEY_I)) {
			SetUIMode(!m_uiMode);
		}
		if (!m_uiMode) {
			UpdateCamera(delta_time_s);
		}
	}

	void LateUpdate() {
		Super::LateUpdate();
	}

	bool Initialize() override;
	void Shutdown() override;

	FrameUniforms UpdateFrameUniforms(uint32_t currentFrame, smvk::Extent2D renderExtent);
	smvk::GpuDirectionalLightDescription UpdateLights(uint32_t currentFrame);
	void UpdateMaterials(uint32_t currentFrame);

	// Emissive point-light ball objects (lights as real scene nodes).
	void CreatePointLightBalls();
	void FixupPointLightBallNodes();
	void EnsurePointLightBalls(uint32_t count);
	void UpdatePointLightBalls();
	uint32_t UpdateForwardPlusLights(uint32_t currentFrame, const FrameUniforms& frame);

	void RebuildSwapViews();
	void DrawFrame() override;
	void RenderFrame(smvk::Framebuffer fb);

	// Error-driven LOD selection (see Engine/Asset/mesh_optimizer.h): the
	// scene references LOD0; cooked "/lodN" siblings are picked per instance
	// from their world-space simplifier error projected to pixels at the
	// instance distance. Returns the LOD mesh (LOD0 fallback when disabled
	// or uncooked). `outLod` receives the selected index for stats.
	smvk::StaticMesh* LodMeshForNode(NodeId node, uint32_t worldMesh,
		uint32_t* outLod = nullptr);
	// Cheap LOD index for filter/direct split decisions (cached chains, no
	// mesh fetch). Returns 0 when LODs are disabled or uncooked.
	uint32_t LodIndexForNode(NodeId node, uint32_t worldMesh) const;
	void ResetLodStats();
	// Runtime LOD switch: pixel budget for the projected simplifier error
	// (~1px = conservative, higher = more aggressive). Disabled = always LOD0.
	bool m_lodEnabled = true;
	float m_lodPixelBudget = 1.0f;
	float m_lodViewportHeight = 1080.0f; // refreshed per frame from renderExtent
	uint64_t m_lodDraws = 0;     // direct draws this frame
	uint64_t m_lodSwitched = 0;  // ... served by LOD1+
	uint32_t m_lodChainMeshes = 0; // world meshes with a cooked chain (per frame)
	uint32_t m_lodChainLods = 0;   // total cooked siblings (per frame)

	static constexpr float default_near_clip = 0.01f;
	static constexpr float kCameraFovYDegrees = 45.0f;

	SceneDesc m_scene{};
	ScenePassConfig m_passes{};
	bool m_forceGltf = false;

	std::unique_ptr<smvk::ImGuiOverlay> m_imgui{};
	AppResources m_resources{};
	ViewportResources m_viewportResources{};
	ForwardPlusDemo m_fpDemo{};
	smvk::CookieLibrary m_cookies{}; // baked/engine-resource gobos (see cookies.h)
	uint32_t m_spotShadowCount = 0; // shadowed spots this frame (tiles used)
	int32_t m_spotShadowMapIndex = -1; // bindless spot atlas, -1 = none
	std::vector<smvk::GpuCascadeDescription> m_spotShadowTiles; // spot tiles this frame (feed the spot RenderViews)
	smvk::RenderViewSet m_renderViews{}; // per-frame virtual cameras: main view + shadow cascade/spot views
	// ---- GeometryFX-style per-view filter (clustered, see App/renderer.h) ----
	app::GfxRenderer m_gfxRenderer{};
	bool m_useGfxFilter = true; // false = legacy direct draws (A/B)
	bool m_useFilteredMotion = true; // false = motion stays fully direct
	// ---- Meshlet/cone debug views (see App/renderer.h debug programs) ----
	bool m_debugMeshlets = false; // meshlet colors replace shaded opaque
	int m_debugMeshletMode = 0;   // 0 = random per meshlet, 1 = cone heat
	bool m_debugCones = false;    // cone-axis line overlay (CPU batches)
	smvk::Holder<smvk::BufferHandle> m_debugLines; // pos-only cone line VB
	uint32_t m_debugLinesCap = 0; // verts allocated
	bool m_dumpIndirectAudit = false; // one-shot: dump GPU indirect cmds (stderr)
	bool m_debugDumpHzb = false;    // one-shot: dump HZB buffer contents (stderr)
	bool m_dumpFirstFrameHzb = false; // opt-in: dump HZB once on the next frame (stalls)
	// ---- Debug culling freeze: hold the filter's (view, eye) + HZB while
	// the render camera moves freely, so culling decisions stay put and
	// their effect (missing survivors, holes) is inspectable. Captured on
	// the rising edge; auto-released on extent drift (a resized HZB would
	// rebuild empty while held -> garbage reads).
	bool m_freezeCullingView = false;
	bool m_frozenViewValid = false;
	glm::mat4 m_frozenViewProj{ 1.0f };
	glm::vec3 m_frozenEye{ 0.0f };
	smvk::Extent2D m_frozenExtent{ 0, 0 };
	app::ForwardPlusRenderer m_fpRenderer{};
	// ---- Motion vector pass: previous-frame state + debug view ----
	glm::mat4 m_prevViewProj{ 1.0f }; // last frame's viewProj (post Y-flip)
	bool m_prevViewProjValid = false; // false until the first frame completes
	std::vector<glm::mat4> m_prevGlobalTransforms; // last frame's node transforms
	bool m_showMotionDebug = false; // tonemap debug view: motion vectors
	float m_motionDebugScale = 16.0f; // debug view velocity amplification
	// ---- Motion blur pass (HDR velocity gather, see motionblur.h) ----
	bool m_motionBlurEnabled = true; // runs after Main, before Bloom/Tonemap
	int m_motionBlurSamples = 12; // gather taps 1..32
	float m_motionBlurMaxRadius = 24.0f; // blur clamp (px)
	float m_motionBlurStrength = 1.0f; // shutter scale
	// ---- SSAO pass (half-res depth-only AO, see ssao.h) ----
	bool m_ssaoEnabled = true; // runs before Main, composites in opaque ambient
	float m_ssaoRadius = 0.5f; // view-space sample radius (m)
	float m_ssaoBias = 0.025f; // self-occlusion bias
	float m_ssaoIntensity = 1.0f; // ambient composite strength (default.frag)
	float m_ssaoMaxDistance = 25.0f; // fade-out distance (m)
	int m_ssaoSamples = 16; // hemisphere taps 1..32
	float m_ssaoBlurSharpness = 8.0f; // blur depth edge weight
	// ---- Emissive point-light ball objects (scene-hierarchy lights) ----
	smvk::Cluster* m_lightCluster = nullptr; // owns the shared ball resources
	smvk::ClusterStaticMeshHandle m_ballMesh{};
	smvk::ClusterMaterialHandle m_ballMatWarm{};
	smvk::ClusterMaterialHandle m_ballMatCool{};
	uint32_t m_ballMeshIdx = UINT32_MAX;      // world-level slot (post-merge)
	uint32_t m_ballMatWarmIdx = UINT32_MAX;   // world-level slot (post-merge)
	uint32_t m_ballMatCoolIdx = UINT32_MAX;   // world-level slot (post-merge)
	int m_lightRootNode = -1;                 // "PointLights" hierarchy root
	std::vector<PointLightBall> m_lightBalls; // node pool (grows on demand)
	uint32_t m_activePointBalls = 0;          // active pool prefix this frame
	smvk::EnvironmentSettings m_env{};
	smvk::Skydome m_skydome{};
	CameraPositioner_FirstPerson m_cameraPositioner = CameraPositioner_FirstPerson(
		glm::vec3(0.0f, 1.0f, 0.0f), //pos
		glm::vec3(0.0f, 1.0f, 1.0f), //target
		glm::vec3(0.0f, 1.0f, 0.0f) //up
	);
	Camera m_camera = Camera(m_cameraPositioner);
};

} // namespace app
