// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// RenderView – a virtual camera + its associated render target.
//
// A RenderView is the single unit of "something that can render":
//   • the main game camera  -> 1 view,
//   • each directional light -> 1 view per shadow cascade (4 for the
//     standard CSM setup, one per atlas tile),
//   • each shadowed spot light -> 1 view (perspective tile),
//   • any other custom pass (reflection probe, security camera, ...).
//
// Views are allocated per frame, uploaded into one storage slab and read
// through a device address (PushConstants::renderViews + view indices),
// so every pass resolves its view-projection the same way instead of
// hand-rolling per-pass matrices. The CPU side keeps the frustum planes
// for culling (see RenderViewIntersectsSphere).
//
// The GPU struct is byte-compatible with shaders/renderview.hlsli
// (DXC -fvk-use-dx-layout); both sides must stay in sync.

#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/light.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace smvk {

// HLSL parity marker: C++ side of the HLSL_ALIGN_STRUCT annotation used in
// renderview.hlsli. Structs carrying it must match their HLSL counterpart
// byte-for-byte and static_assert their size.
#define HLSL_ALIGN_STRUCT alignas(16)

// What a view is used for (mirrors RENDERVIEW_FLAG_* in renderview.hlsli).
typedef enum RenderViewFlagBits {
	RenderViewFlagBit_None = 0,
	RenderViewFlagBit_MainView = 1u << 0,      // game camera
	RenderViewFlagBit_ShadowCascade = 1u << 1, // directional light CSM cascade
	RenderViewFlagBit_SpotShadow = 1u << 2,    // spot light shadow tile
	RenderViewFlagBit_Custom = 1u << 3,        // reflection/mini-map/other
	RenderViewFlagBit_PointShadow = 1u << 4,   // point light shadow face tile
} RenderViewFlagBits;
typedef uint32_t RenderViewFlags;

// One virtual camera + its render target (256 bytes).
// Layout must match HLSL renderview.hlsli::RenderView byte-for-byte.
struct HLSL_ALIGN_STRUCT RenderView {
	glm::mat4 m_viewProjectionMatrix;        // 0
	glm::mat4 m_inverseViewProjectionMatrix; // 64
	glm::vec4 m_viewPlanes[6];               // 128: inward frustum planes (xyz normal, w distance)
	glm::vec4 m_renderTargetSize;           // 224: xy = target size (px), zw = 1/size
	uint32_t  m_renderViewFlags = RenderViewFlagBit_None; // 240
	glm::uvec3 m_padding0 = {};             // 244
};
static_assert(sizeof(RenderView) == 256, "RenderView must match HLSL renderview.hlsli");
static_assert(sizeof(RenderView) % 16 == 0);

// ---------------------------------------------------------------------------
// Frustum planes
// ---------------------------------------------------------------------------

// Gribb-Hartmann plane extraction for GLM column-major matrices
// (clip = vp * pos) with Vulkan zero-to-one depth (z_ndc in [0,1]).
// Works for standard and reversed-Z (infinite far included) projections:
// the six resulting half-spaces always describe the visible volume. Planes
// are normalized; a point p is inside when
// dot(plane.xyz, p) + plane.w >= 0 for all six planes.
//
// guardNdcX/Y dilate the left/right and bottom/top half-spaces by that many
// NDC units (callers convert pixels via 2*guardPx/extent). The dilation is
// strictly conservative: the inside set only grows, so culling keeps a
// boundary band instead of dropping it. Used by jittered views (SMAA T2x
// swings the frustum by +/-0.25 px each frame): without a guard,
// boundary clusters flicker in/out on alternating frames. Near/far stay
// exact (subpixel jitter never moves depth).
inline void ExtractViewPlanes(const glm::mat4& vp, glm::vec4 outPlanes[6],
	float guardNdcX = 0.0f, float guardNdcY = 0.0f)
{
	const auto row = [&vp](int r) {
		return glm::vec4(vp[0][r], vp[1][r], vp[2][r], vp[3][r]);
	};
	const glm::vec4 row0 = row(0);
	const glm::vec4 row1 = row(1);
	const glm::vec4 row2 = row(2);
	const glm::vec4 row3 = row(3);

	glm::vec4 planes[6] = {
		row3 * (1.0f + guardNdcX) + row0, // left   (x_ndc = -1)
		row3 * (1.0f + guardNdcX) - row0, // right  (x_ndc = +1)
		row3 * (1.0f + guardNdcY) + row1, // bottom (y_ndc = -1)
		row3 * (1.0f + guardNdcY) - row1, // top    (y_ndc = +1)
		row2,        // z_ndc = 0 (near for ZO, far for reversed-Z)
		row3 - row2, // z_ndc = 1 (far for ZO, near for reversed-Z)
	};
	for (glm::vec4& p : planes) {
		const float len = glm::length(glm::vec3(p));
		if (len > 1e-8f)
			p /= len;
	}
	std::memcpy(outPlanes, planes, sizeof(planes));
}

// Conservative world-space bounding sphere of a mesh AABB under `model`.
inline void MeshBoundsToWorld(
	const glm::mat4& model,
	const glm::vec3& aabbMin,
	const glm::vec3& aabbMax,
	glm::vec3& outCenter,
	float& outRadius)
{
	outCenter = glm::vec3(model * glm::vec4((aabbMin + aabbMax) * 0.5f, 1.0f));
	const glm::vec3 halfDiagonal = (aabbMax - aabbMin) * 0.5f;
	const float maxScale = std::max({
		glm::length(glm::vec3(model[0])),
		glm::length(glm::vec3(model[1])),
		glm::length(glm::vec3(model[2])) });
	outRadius = glm::length(halfDiagonal) * maxScale;
}

// Sphere-vs-view-planes test (world space). True when the sphere may be
// visible in the view; false only when fully outside one plane.
inline bool RenderViewIntersectsSphere(
	const RenderView& view,
	const glm::vec3& center,
	float radius)
{
	for (uint32_t i = 0; i < 6; ++i) {
		const glm::vec4& p = view.m_viewPlanes[i];
		if (glm::dot(glm::vec3(p), center) + p.w < -radius)
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Shadow-caster contribution volume (directional lights)
// ---------------------------------------------------------------------------

// The SHADE volume of a view for a directional light: the view's frustum
// swept along -lightDir (toward the sun, where the casters live). A caster
// anywhere in this volume can shadow SOMETHING inside the original frustum;
// a caster outside it cannot (for this light direction).
//
// Plane math: sweeping a convex body A along a ray s keeps half-space
// H(n, w) valid iff dot(n, s) >= 0 (swept points only drift deeper into
// it); planes with dot(n, s) < 0 are EXITED by the sweep and stop
// constraining the volume. With s = -lightDir that drops exactly the
// SUNWARD-FACING planes: the volume opens toward the sun while the
// down-sun sides stay bounded (a straight-down sun opens the top plane -
// a roof above the camera must stay - and keeps the bottom).
//
// Why this exists: testing casters against the RAW main frustum (the old
// contribution cull) dropped every caster outside the camera that still
// cast INTO it - an oblique sun throws off-screen geometry's shadows
// across the view, and those shadows popped in/out at the screen edges as
// the camera moved casters across the frustum boundary. This swept volume
// is the conservative replacement: it only drops casters that cannot
// reach the view at all.
//
// Dropped planes are ZEROED instead of removed so the result stays a
// RenderView and RenderViewIntersectsSphere works unmodified: a zeroed
// plane (normal 0, w 0) never culls (dot(0, c) + 0 = 0 >= -radius). The
// matrices are copied from the source view but never read - only the
// planes matter. A degenerate lightDir keeps the raw planes (never worse
// than the raw frustum).
inline RenderView MakeCasterContributionView(
	const RenderView& mainView,
	const glm::vec3& lightDir)
{
	RenderView out = mainView;
	if (glm::length(lightDir) < 1e-4f)
		return out;
	const glm::vec3 d = glm::normalize(lightDir);
	for (uint32_t i = 0; i < 6; ++i) {
		const glm::vec3 n(out.m_viewPlanes[i]);
		if (glm::dot(n, d) > 0.0f)
			out.m_viewPlanes[i] = glm::vec4(0.0f);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Per-frame allocator + GPU upload
// ---------------------------------------------------------------------------

// Mirrors MAX_RENDER_VIEWS in renderview.hlsli.
static constexpr uint32_t kMaxRenderViews = 64;
// Cascades per directional light (GpuDirectionalLightDescription::cascades[4]).
static constexpr uint32_t kMaxShadowCascadesPerLight = 4;

// Frame flow: NewFrame() -> Allocate*() (main camera first, then shadow /
// custom views) -> Upload() -> DeviceAddress()/MainViewIndex() feed the push
// constants. Indices stay stable until the next NewFrame().
class RenderViewSet {
public:
	static constexpr uint32_t kInvalidIndex = UINT32_MAX;

	// Creates the GPU slab (kMaxRenderViews * framesInFlight). Idempotent.
	bool Ensure(ResourceManager& ctx, uint32_t framesInFlight, const char* name = "render views") {
		if (Valid() && framesInFlight_ == framesInFlight)
			return true;
		buffer_ = ctx.CreateBuffer(
			sizeof(RenderView) * kMaxRenderViews * framesInFlight,
			BufferType_Storage,
			name);
		framesInFlight_ = buffer_.empty() ? 0u : framesInFlight;
		if (!Valid())
			count_ = 0;
		return Valid();
	}

	void Destroy() {
		buffer_ = {};
		framesInFlight_ = 0;
		NewFrame();
	}

	[[nodiscard]] bool Valid() const { return !buffer_.empty(); }

	// ---------------------------------------------------------------
	// CPU-side per-frame allocation
	// ---------------------------------------------------------------

	void NewFrame() {
		count_ = 0;
		mainView_ = kInvalidIndex;
	}

	[[nodiscard]] uint32_t Count() const { return count_; }
	[[nodiscard]] uint32_t MainViewIndex() const { return mainView_; }

	[[nodiscard]] const RenderView& Get(uint32_t index) const {
		assert(index < count_);
		return views_[index];
	}

	// Generic allocation (world-space viewProj + target size in pixels).
	// cullingGuardPx dilates the frustum planes by that many target pixels
	// (see ExtractViewPlanes): pass > 0 for views whose projection jitters
	// per frame (main camera under SMAA T2x), 0 for exact views (shadows).
	uint32_t AllocateView(
		const glm::mat4& viewProj,
		const glm::vec2& targetSize,
		RenderViewFlags flags,
		float cullingGuardPx = 0.0f) {
		if (count_ >= kMaxRenderViews)
			return kInvalidIndex;
		RenderView v;
		std::memset(&v, 0, sizeof(v));
		v.m_viewProjectionMatrix = viewProj;
		v.m_inverseViewProjectionMatrix = glm::inverse(viewProj);
		const float guardNdcX = (targetSize.x > 0.0f && cullingGuardPx > 0.0f)
			? 2.0f * cullingGuardPx / targetSize.x : 0.0f;
		const float guardNdcY = (targetSize.y > 0.0f && cullingGuardPx > 0.0f)
			? 2.0f * cullingGuardPx / targetSize.y : 0.0f;
		ExtractViewPlanes(viewProj, v.m_viewPlanes, guardNdcX, guardNdcY);
		v.m_renderTargetSize = glm::vec4(
			targetSize.x, targetSize.y,
			targetSize.x > 0.0f ? 1.0f / targetSize.x : 0.0f,
			targetSize.y > 0.0f ? 1.0f / targetSize.y : 0.0f);
		v.m_renderViewFlags = flags;
		views_[count_] = v;
		return count_++;
	}

	// Main game camera -> exactly 1 view (first allocation wins MainViewIndex).
	uint32_t AllocateMainView(
		const glm::mat4& viewProj,
		const glm::vec2& targetSize,
		float cullingGuardPx = 0.0f) {
		const uint32_t index = AllocateView(viewProj, targetSize, RenderViewFlagBit_MainView, cullingGuardPx);
		if (mainView_ == kInvalidIndex)
			mainView_ = index;
		return index;
	}

	// One directional light -> 1 view per shadow cascade. VPs come from the
	// built GpuDirectionalLightDescription (same matrices the shadow passes
	// and the receiver sampling use). Returns the first cascade view index
	// (cascade i at base + i), kInvalidIndex when the light has no cascades.
	uint32_t AllocateDirectionalLightViews(
		const GpuDirectionalLightDescription& dir,
		const glm::vec2& tileSize) {
		const uint32_t cascadeCount = std::min<uint32_t>(
			std::max<uint32_t>(static_cast<uint32_t>(dir.numCascades), 0u),
			kMaxShadowCascadesPerLight);
		if (cascadeCount == 0)
			return kInvalidIndex;
		const uint32_t base = AllocateView(
			glm::make_mat4(dir.cascades[0].lightVP), tileSize, RenderViewFlagBit_ShadowCascade);
		for (uint32_t i = 1; i < cascadeCount; ++i) {
			AllocateView(
				glm::make_mat4(dir.cascades[i].lightVP), tileSize, RenderViewFlagBit_ShadowCascade);
		}
		return base;
	}

	// One shadowed spot light -> 1 view (VP from its atlas tile descriptor).
	uint32_t AllocateSpotShadowView(
		const GpuCascadeDescription& tile,
		const glm::vec2& tileSize) {
		return AllocateView(
			glm::make_mat4(tile.lightVP), tileSize, RenderViewFlagBit_SpotShadow);
	}

	// One shadowed point light -> 6 views (one per cube-face tile; the
	// caller allocates them back-to-back so face f is base + f, matching
	// the frame's consecutive tile numbering - see spotshadow.h).
	uint32_t AllocatePointShadowFaceView(
		const GpuCascadeDescription& tile,
		const glm::vec2& tileSize) {
		return AllocateView(
			glm::make_mat4(tile.lightVP), tileSize, RenderViewFlagBit_PointShadow);
	}

	// Any other custom pass.
	uint32_t AllocateCustomView(
		const glm::mat4& viewProj,
		const glm::vec2& targetSize,
		float cullingGuardPx = 0.0f) {
		return AllocateView(
			viewProj, targetSize, RenderViewFlagBit_Custom, cullingGuardPx);
	}

	// ---------------------------------------------------------------
	// GPU upload / access
	// ---------------------------------------------------------------

	// Uploads the full frame slice (unused slots zeroed) so stale indices
	// never observe garbage.
	void Upload(ResourceManager& ctx, uint32_t frameIndex) {
		assert(Valid());
		RenderView slab[kMaxRenderViews];
		std::memset(slab, 0, sizeof(slab));
		std::memcpy(slab, views_.data(), sizeof(RenderView) * count_);
		constexpr size_t frameStride = sizeof(RenderView) * kMaxRenderViews;
		ctx.UploadDataToBuffer(
			buffer_,
			slab,
			frameStride,
			PipelineStage(PipelineStageBit_Vertex | PipelineStageBit_Fragment),
			AccessFlags::ShaderRead,
			0,
			frameIndex * frameStride);
	}

	// Device address of the frame's slice (for PushConstants::renderViews).
	[[nodiscard]] uint64_t DeviceAddress(ResourceManager& ctx, uint32_t frameIndex) const {
		assert(Valid());
		constexpr size_t frameStride = sizeof(RenderView) * kMaxRenderViews;
		return ctx.GetBufferDeviceAddress(buffer_) + frameIndex * frameStride;
	}

	// Debug: GPU slab download + view-matrix dump (z-row) for the given
	// frame slice. Useful to verify the uploaded views match the CPU side.
	void DumpSlabViews(ResourceManager& ctx, uint32_t frameIndex,
		uint32_t first, uint32_t count) const {
		constexpr size_t frameStride = sizeof(RenderView) * kMaxRenderViews;
		std::vector<uint8_t> bytes(sizeof(RenderView) * kMaxRenderViews, 0);
		if (!ctx.DownloadDataFromBuffer(buffer_, bytes.data(), bytes.size(),
				frameIndex * frameStride)) {
			std::fprintf(stderr, "[slab-gpu] download failed\n");
			return;
		}
		for (uint32_t i = first; i < first + count; ++i) {
			const float* m = reinterpret_cast<const float*>(
				bytes.data() + i * sizeof(RenderView));
			std::fprintf(stderr,
				"[slab-gpu %u] vp row2=[%.4g %.4g %.4g %.4g]\n",
				i, m[2], m[6], m[10], m[14]);
		}
	}

	// Debug: CPU-side view matrix dump.
	void DumpViews(uint32_t first, uint32_t count) const {
		for (uint32_t i = first; i < first + count && i < count_; ++i) {
			const float* m = glm::value_ptr(views_[i].m_viewProjectionMatrix);
			std::fprintf(stderr,
				"[views cpu %u] vp=[%.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g]\n",
				i, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
				m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
		}
	}

private:
	Holder<BufferHandle> buffer_;
	std::vector<RenderView> views_ = std::vector<RenderView>(kMaxRenderViews);
	uint32_t framesInFlight_ = 0;
	uint32_t count_ = 0;
	uint32_t mainView_ = kInvalidIndex;
};

} // namespace smvk
