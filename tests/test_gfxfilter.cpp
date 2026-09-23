// gfxfilter_tests: CPU-side tests for the GeometryFX-style per-view
// filter (Engine/Renderer/gfx_filter.h). No GPU required: registration,
// batching and the CPU pre-cull are pure CPU code paths, and the winding
// experiment below mirrors the shader's homogeneous backface formula
// against geometric ground truth.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Engine/Renderer/gfx_filter.h>
#include <Engine/debug_log.h>
// Detail diagnostics (winding/cull/spawn/mirror traces) print in debug
// builds only; PASS/FAIL lines and the final summary always print.
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/renderview.h>
#include <Engine/Camera/camera.h>

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

// ---------------------------------------------------------------------------
// UV sphere with outward (geometric) winding, self-checked.
// Pole rings are degenerate by construction and skipped by consumers.
// ---------------------------------------------------------------------------

smvk::StaticMeshUpload MakeSphere(int rings, int segments, float radius) {
	smvk::StaticMeshUpload out{};
	for (int r = 0; r <= rings; ++r) {
		const float theta = (float)r / (float)rings * glm::pi<float>();
		const float y = std::cos(theta) * radius;
		const float ringR = std::sin(theta) * radius;
		for (int s = 0; s <= segments; ++s) {
			const float phi = (float)s / (float)segments * 2.0f * glm::pi<float>();
			smvk::StaticVertex v{};
			v.pos = glm::vec3(std::cos(phi) * ringR, y, std::sin(phi) * ringR);
			v.normal = ringR > 1e-9f ? glm::normalize(v.pos) : glm::vec3(0, theta < 1.0f ? 1.0f : -1.0f, 0);
			out.vertices.push_back(v);
		}
	}
	const int stride = segments + 1;
	for (int r = 0; r < rings; ++r) {
		for (int s = 0; s < segments; ++s) {
			const uint32_t a = (uint32_t)(r * stride + s);
			const uint32_t b = (uint32_t)(r * stride + s + 1);
			const uint32_t c = (uint32_t)((r + 1) * stride + s);
			const uint32_t d = (uint32_t)((r + 1) * stride + s + 1);
			// Outward winding (verified by CheckOutwardWinding below).
			out.indices.insert(out.indices.end(), { a, b, c, b, d, c });
		}
	}
	return out;
}

static float TriArea2(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
	return glm::length(glm::cross(b - a, c - a));
}

// Pole slivers (sin(pi) != 0 in float) have areas ~1e-8: pure noise.
// Smallest real triangle here is ~1e-2, so 1e-6 separates cleanly.
static constexpr float kDegenerateArea = 1e-6f;

bool CheckOutwardWinding(const smvk::StaticMeshUpload& up) {
	for (size_t t = 0; t < up.indices.size(); t += 3) {
		const glm::vec3& a = up.vertices[up.indices[t + 0]].pos;
		const glm::vec3& b = up.vertices[up.indices[t + 1]].pos;
		const glm::vec3& c = up.vertices[up.indices[t + 2]].pos;
		if (TriArea2(a, b, c) < kDegenerateArea)
			continue; // degenerate pole triangles/slivers
		const glm::vec3 n = glm::cross(b - a, c - a);
		const glm::vec3 center = (a + b + c) / 3.0f;
		if (glm::dot(n, center) <= 0.0f) {
#ifndef NDEBUG
			std::cout << "[gfx.wind] inward tri " << (t / 3)
				<< " a=(" << a.x << "," << a.y << "," << a.z << ")"
				<< " b=(" << b.x << "," << b.y << "," << b.z << ")"
				<< " c=(" << c.x << "," << c.y << "," << c.z << ")"
				<< " dot=" << glm::dot(n, center) << "\n";
#endif
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// T1: winding-sign agreement.
//
// Mirrors gfxfilter.cs.hlsl CullTriangle's backface term
//   back = determinant(rows(clip[i].xyw)) > 0   (+ optional flip)
// against geometric ground truth (normal facing the camera), using the
// engine's exact projection convention (reversed infinite-Z RH +
// proj[1][1] *= -1, cf. UpdateFrameUniforms in app/src/main.cpp).
//
// Near-silhouette triangles are excluded (|cos| threshold): both methods
// are exact away from the silhouette and may differ within it.
// ---------------------------------------------------------------------------

void TestWindingSign() {
	const auto up = MakeSphere(16, 24, 1.0f);
	Check(CheckOutwardWinding(up), "gfx.wind.sphere-outward");

	const glm::vec3 eye(0.0f, 0.0f, 4.0f);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f; // viewport fix, mirrors UpdateFrameUniforms
	const glm::mat4 viewProj = proj * view;

	uint32_t counted = 0, agreeFlip = 0, agreeNoFlip = 0, total = 0;
	uint32_t geoPixelAgree = 0, detPixelFrontNoFlip = 0, detPixelFrontFlip = 0;
	for (size_t t = 0; t < up.indices.size(); t += 3) {
		const glm::vec3& a = up.vertices[up.indices[t + 0]].pos;
		const glm::vec3& b = up.vertices[up.indices[t + 1]].pos;
		const glm::vec3& c = up.vertices[up.indices[t + 2]].pos;
		++total;
		if (TriArea2(a, b, c) < kDegenerateArea)
			continue; // degenerate pole triangles/slivers
		const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
		const glm::vec3 center = (a + b + c) / 3.0f;
		const float facing = glm::dot(n, glm::normalize(center - eye));
		if (std::abs(facing) < 0.15f)
			continue; // silhouette band: methods may legitimately differ
		++counted;
		const bool front = facing < 0.0f;

		const glm::vec4 ca = viewProj * glm::vec4(a, 1.0f);
		const glm::vec4 cb = viewProj * glm::vec4(b, 1.0f);
		const glm::vec4 cc = viewProj * glm::vec4(c, 1.0f);
		if (ca.w <= 0.0f || cb.w <= 0.0f || cc.w <= 0.0f)
			continue; // behind camera: not rasterized, out of experiment
		// Same expression as the shader (det sign is transpose-invariant).
		const glm::mat3 m(
			glm::vec3(ca.x, ca.y, ca.w),
			glm::vec3(cb.x, cb.y, cb.w),
			glm::vec3(cc.x, cc.y, cc.w));
		const bool backNoFlip = glm::determinant(m) > 0.0f;
		if ((front && backNoFlip) || (!front && !backNoFlip))
			++agreeFlip; // flipped: keep = backNoFlip
		if ((front && !backNoFlip) || (!front && backNoFlip))
			++agreeNoFlip;

		// Rasterizer-anchored link: pixels are y-down; visually-CCW (what
		// FrontFace::CounterClockwise keeps) has negative math signed area.
		constexpr float W = 800.0f, H = 600.0f;
		const glm::vec2 pa(ca.x / ca.w * 0.5f + 0.5f, ca.y / ca.w * 0.5f + 0.5f);
		const glm::vec2 pb(cb.x / cb.w * 0.5f + 0.5f, cb.y / cb.w * 0.5f + 0.5f);
		const glm::vec2 pc2(cc.x / cc.w * 0.5f + 0.5f, cc.y / cc.w * 0.5f + 0.5f);
		(void)W;
		(void)H;
		const float area2 = (pb.x - pa.x) * (pc2.y - pa.y) - (pc2.x - pa.x) * (pb.y - pa.y);
		const bool pixelFront = area2 < 0.0f; // visual CCW in y-down pixels
		if (front == pixelFront)
			++geoPixelAgree;
		if ((!backNoFlip) == pixelFront)
			++detPixelFrontNoFlip;
		if (backNoFlip == pixelFront)
			++detPixelFrontFlip;
	}

	const double fracFlip = counted ? (double)agreeFlip / (double)counted : 0.0;
	const double fracNoFlip = counted ? (double)agreeNoFlip / (double)counted : 0.0;
	const double fracGeoPixel = counted ? (double)geoPixelAgree / (double)counted : 0.0;
	const double fracDetNoFlip = counted ? (double)detPixelFrontNoFlip / (double)counted : 0.0;
	const double fracDetFlip = counted ? (double)detPixelFrontFlip / (double)counted : 0.0;
#ifndef NDEBUG
	std::cout << "[gfx.wind] tris=" << total << " counted=" << counted
		<< " agreeFlip=" << fracFlip << " agreeNoFlip=" << fracNoFlip
		<< " geoPixel=" << fracGeoPixel
		<< " detPixelNoFlip=" << fracDetNoFlip << " detPixelFlip=" << fracDetFlip << "\n";
#endif
	Check(counted > total * 7 / 10, "gfx.wind.meaningful-subset");
	Check(fracGeoPixel > 0.98, "gfx.wind.ground-truth-matches-rasterizer");
	// Exactly one of these must hold; whichever does selects the flag.
	const bool noFlipRight = fracDetNoFlip > 0.98 && fracDetFlip < 0.05;
	const bool flipRight = fracDetFlip > 0.98 && fracDetNoFlip < 0.05;
#ifndef NDEBUG
	std::cout << "[gfx.wind] noFlipRight=" << noFlipRight << " flipRight=" << flipRight << "\n";
#endif
	Check(noFlipRight != flipRight, "gfx.wind.unambiguous");
	Check(noFlipRight, "gfx.wind.noflip-matches-rasterizer");
}

// ---------------------------------------------------------------------------
// T2: registration preserves the mesh (cluster cover, id mapping).
// ---------------------------------------------------------------------------

void TestRegistration() {
	using namespace smvk::gfx;
	State s{};
	const auto up = MakeSphere(8, 12, 1.0f);
	const uint32_t baseVertex = 17; // nonzero: exercises the global-id lift
	const uint32_t mesh = RegisterMeshFromUpload(s, up, baseVertex);
	Check(mesh == 0, "gfx.reg.mesh-index");
	Check(!s.clusters.empty(), "gfx.reg.has-clusters");

	uint32_t triSum = 0;
	bool idsOk = true, sizeOk = true;
	const uint32_t meshTris = (uint32_t)up.indices.size() / 3;
	for (const Cluster& c : s.clusters) {
		triSum += c.triCount;
		if (c.triCount == 0 || c.triCount > kMaxClusterTris)
			sizeOk = false;
		if (c.indexOffset + c.triCount * 3 > (uint32_t)s.filterIndices.size())
			sizeOk = false;
		for (uint32_t k = 0; k < c.triCount * 3; ++k) {
			const uint32_t id = s.filterIndices[c.indexOffset + k];
			if (id < baseVertex || id >= baseVertex + (uint32_t)up.vertices.size())
				idsOk = false;
		}
	}
	Check(triSum == meshTris, "gfx.reg.tri-cover");
	Check(idsOk, "gfx.reg.global-ids-in-range");
	Check(sizeOk, "gfx.reg.cluster-size-bounds");
	Check(s.meshes.size() == 1 && s.meshes[0].clusterCount == (uint32_t)s.clusters.size(),
		"gfx.reg.mesh-range");
	Check(s.filterIndices.size() == up.indices.size(), "gfx.reg.index-count-preserved");
}

// ---------------------------------------------------------------------------
// T3: batching (pure CPU BuildBatches on a hand-built view, no GPU).
// ---------------------------------------------------------------------------

smvk::StaticMeshUpload MakeTri(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
	smvk::StaticMeshUpload out{};
	smvk::StaticVertex va{}, vb{}, vc{};
	va.pos = a;
	vb.pos = b;
	vc.pos = c;
	out.vertices = { va, vb, vc };
	out.indices = { 0, 1, 2 };
	return out;
}

void TestBatching() {
	using namespace smvk::gfx;
	State s{};
	// Two single-triangle meshes (each cooks to exactly 1 cluster).
	const uint32_t m0 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)), 0);
	const uint32_t m1 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(2, 0, 0), glm::vec3(3, 0, 0), glm::vec3(2, 1, 0)), 3);
	Check(m0 == 0 && m1 == 1, "gfx.batch.mesh-ids");
	Check(s.clusters.size() == 2, "gfx.batch.two-clusters");

	// Camera in front (+Z looking -Z): both front-facing, fully in frustum.
	smvk::RenderViewSet views;
	views.NewFrame();
	const glm::vec3 eye(1.5f, 0.5f, 5.0f);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(1.5f, 0.5f, 0.0f), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f;
	const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });

	View v{};
	v.renderView = &views.Get(rv);
	v.eye = eye;
	v.width = 800;
	v.height = 600;
	v.viewProj = views.Get(rv).m_viewProjectionMatrix;
	v.cullFlags = CullFlagBit_Degenerate;
	s.views.push_back(std::move(v));
	s.viewCount = 1;

	// 3 draws, 2 materials (order shuffled to test material sorting).
	PushDraw(s, 0, m1, glm::mat4(1.0f), 7);
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 3);
	BuildBatches(s, 0);

	const View& out = s.views[0];
	Check(out.batchCount == 3, "gfx.batch.count");
	Check(out.stats.trianglesProcessed == 3, "gfx.batch.tris");
	Check(out.stats.commands == 3, "gfx.batch.commands");
	Check(out.drawConstants.size() == 3, "gfx.batch.drawconsts");

	// Ranges: contiguous, cover all commands, sorted by material; every
	// batch header references a valid cluster/draw/mesh.
	uint32_t covered = 0;
	bool sorted = true, refsOk = true;
	uint32_t prevMat = 0;
	for (size_t i = 0; i < out.materialRanges.size(); ++i) {
		const MaterialRange& r = out.materialRanges[i];
		if (i > 0 && r.material < prevMat)
			sorted = false;
		if (r.firstCommand != covered)
			refsOk = false;
		covered += r.commandCount;
		prevMat = r.material;
		for (uint32_t k = 0; k < r.commandCount; ++k) {
			const Batch& b = out.batches[r.firstCommand + k];
			if (b.cluster >= s.clusters.size() || b.draw >= out.drawConstants.size())
				refsOk = false;
			else if (out.drawConstants[b.draw].mesh >= s.meshes.size())
				refsOk = false;
			if (b.outputOffset != (r.firstCommand + k) * kMaxClusterTris * 3)
				refsOk = false;
		}
	}
	Check(sorted, "gfx.batch.ranges-sorted");
	Check(refsOk, "gfx.batch.refs-valid");
	Check(covered == out.batchCount, "gfx.batch.ranges-cover");
	Check(out.materialRanges.size() == 2, "gfx.batch.two-materials");
	// Default DrawRequests carry no lightmap (0xFFFFFFFFu), so the new
	// lobe grouping must not split these ranges.
	bool lobesDefault = true;
	for (size_t i = 0; i < out.materialRanges.size(); ++i)
		lobesDefault = lobesDefault
			&& out.materialRanges[i].lobeCount == 0xFFFFFFFFu;
	Check(lobesDefault, "gfx.batch.ranges-default-lobes");
}

// ---------------------------------------------------------------------------
// T3b: material ranges split by (material, lightmap lobe class).
//
// The Main filtered leg binds one lit program per range — the generic
// lit FS or the SG_LM_LOBE_COUNT-specialized twin — so draws sharing a
// material but carrying different cooked lobe counts must land in
// different ranges, each stamped with its uniform count.
// ---------------------------------------------------------------------------

void TestLobeGrouping() {
	using namespace smvk::gfx;
	State s{};
	// Two single-triangle meshes (each cooks to exactly 1 cluster).
	const uint32_t m0 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)), 0);
	const uint32_t m1 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(2, 0, 0), glm::vec3(3, 0, 0), glm::vec3(2, 1, 0)), 3);
	Check(m0 == 0 && m1 == 1, "gfx.lobe.mesh-ids");

	// Camera in front (+Z looking -Z): both front-facing, fully in frustum.
	smvk::RenderViewSet views;
	views.NewFrame();
	const glm::vec3 eye(1.5f, 0.5f, 5.0f);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(1.5f, 0.5f, 0.0f), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f;
	const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });

	View v{};
	v.renderView = &views.Get(rv);
	v.eye = eye;
	v.width = 800;
	v.height = 600;
	v.viewProj = views.Get(rv).m_viewProjectionMatrix;
	v.cullFlags = CullFlagBit_Degenerate;
	s.views.push_back(std::move(v));
	s.viewCount = 1;

	// 4 draws, ONE material, THREE lobe classes (shuffled): the range
	// must split by (material, lobe) even though the material is shared.
	PushDraw(s, 0, m1, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque, 6u);
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque, 12u);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque, 0xFFFFFFFFu);
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque, 6u);
	BuildBatches(s, 0);

	const View& out = s.views[0];
	Check(out.batchCount == 4, "gfx.lobe.count");
	const uint32_t expectLobes[3] = { 6u, 12u, 0xFFFFFFFFu };
	const uint32_t expectCmds[3] = { 2, 1, 1 };
	uint32_t covered = 0;
	bool grouped = out.materialRanges.size() == 3;
	for (size_t i = 0; grouped && i < out.materialRanges.size(); ++i) {
		const MaterialRange& r = out.materialRanges[i];
		grouped = grouped
			&& r.material == 3
			&& r.lobeCount == expectLobes[i]
			&& r.commandCount == expectCmds[i]
			&& r.firstCommand == covered;
		for (uint32_t k = 0; grouped && k < r.commandCount; ++k) {
			const Batch& b = out.batches[r.firstCommand + k];
			grouped = grouped
				&& b.draw < out.drawConstants.size()
				&& out.drawConstants[b.draw].lmChunk == r.lobeCount;
		}
		covered += r.commandCount;
	}
	Check(grouped, "gfx.lobe.split");
	Check(covered == out.batchCount, "gfx.lobe.cover");

	// Material stays the major key: (7,6) sorts after every (3,*).
	State s2{};
	const uint32_t n0 = RegisterMeshFromUpload(s2, MakeTri(
		glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)), 0);
	smvk::RenderViewSet views2;
	views2.NewFrame();
	const uint32_t rv2 = views2.AllocateMainView(proj * view, { 800.0f, 600.0f });
	View v2{};
	v2.renderView = &views2.Get(rv2);
	v2.eye = eye;
	v2.width = 800;
	v2.height = 600;
	v2.viewProj = views2.Get(rv2).m_viewProjectionMatrix;
	v2.cullFlags = CullFlagBit_Degenerate;
	s2.views.push_back(std::move(v2));
	s2.viewCount = 1;
	PushDraw(s2, 0, n0, glm::mat4(1.0f), 7, 0, DrawSetBit_Opaque, 6u);
	PushDraw(s2, 0, n0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque, 9u);
	BuildBatches(s2, 0);
	const View& out2 = s2.views[0];
	bool orderOk = out2.materialRanges.size() == 2
		&& out2.materialRanges[0].material == 3
		&& out2.materialRanges[0].lobeCount == 9u
		&& out2.materialRanges[1].material == 7
		&& out2.materialRanges[1].lobeCount == 6u;
	Check(orderOk, "gfx.lobe.material-major");
}

// ---------------------------------------------------------------------------
// T4: pre-cull wiring (sphere + cone) on a real mesh.
//
// ExpectedCull re-implements the decision independently (world-space sphere
// vs planes + cone); agreement with CullCluster guards the
// wiring (spaces, transforms), not just the formula.
// ---------------------------------------------------------------------------

bool ExpectedCull(const smvk::gfx::Cluster& c, const glm::mat4& model,
	const glm::vec3& eye, const smvk::RenderView& rv) {
	const glm::vec3 wc = glm::vec3(model * glm::vec4(c.boundCenter, 1.0f));
	const float s = std::max({
		glm::length(glm::vec3(model[0])),
		glm::length(glm::vec3(model[1])),
		glm::length(glm::vec3(model[2])) });
	const float wr = c.boundRadius * s;
	for (uint32_t i = 0; i < 6; ++i) {
		const glm::vec4& p = rv.m_viewPlanes[i];
		if (glm::dot(glm::vec3(p), wc) + p.w < -wr)
			return true;
	}
	const glm::vec3 eyeObj = glm::vec3(glm::inverse(model) * glm::vec4(eye, 1.0f));
	const glm::vec3 d = c.coneApex - eyeObj;
	const float len = glm::length(d);
	if (len < 1e-6f)
		return false;
	return glm::dot(d / len, c.coneAxis) >= c.coneCutoff;
}

void TestPreCull() {
	using namespace smvk::gfx;
	State s{};
	// Large enough for real meshlets (~15+ clusters so the back hemisphere
	// actually cone-culls; a tiny sphere collapses to a few fat clusters).
	const auto up = MakeSphere(24, 36, 1.0f);
	const uint32_t m = RegisterMeshFromUpload(s, up, 0);
	Check(m == 0, "gfx.cull.mesh-id");

	// Front view: back hemisphere cone-culled, front kept.
	{
		s.views.clear();
		s.viewCount = 0;
		smvk::RenderViewSet views;
		views.NewFrame();
		const glm::vec3 eye(0, 0, 4);
		const glm::mat4 view = glm::lookAt(eye, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
		glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
		proj[1][1] *= -1.0f;
		const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });
		View v{};
		v.renderView = &views.Get(rv);
		v.eye = eye;
		v.width = 800;
		v.height = 600;
		v.viewProj = views.Get(rv).m_viewProjectionMatrix;
		s.views.push_back(std::move(v));
		s.viewCount = 1;
		PushDraw(s, 0, m, glm::mat4(1.0f), 0);
		BuildBatches(s, 0);
		const View& out = s.views[0];
#ifndef NDEBUG
		std::cout << "[gfx.cull] front: kept=" << out.batchCount
			<< " culled=" << out.stats.clustersCulled
			<< " processed=" << out.stats.clustersProcessed << "\n";
#endif
		Check(out.batchCount > 0, "gfx.cull.front-keeps-some");
		Check(out.stats.clustersCulled > 0, "gfx.cull.front-culls-back");
		Check(out.batchCount + out.stats.clustersCulled == out.stats.clustersProcessed,
			"gfx.cull.accounts-all");

		bool wiringOk = true;
		const smvk::RenderView& renderView = views.Get(rv);
		for (const Batch& b : out.batches) {
			if (ExpectedCull(s.clusters[b.cluster], glm::mat4(1.0f), eye, renderView))
				wiringOk = false; // a kept batch must survive independent retest
		}
		Check(wiringOk, "gfx.cull.kept-survive-retest");
	}

	// Looking away: everything outside the frustum -> zero batches.
	{
		s.views.clear();
		s.viewCount = 0;
		smvk::RenderViewSet views;
		views.NewFrame();
		const glm::vec3 eye(0, 0, 4);
		const glm::mat4 view = glm::lookAt(eye, glm::vec3(0, 0, 8), glm::vec3(0, 1, 0));
		glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
		proj[1][1] *= -1.0f;
		const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });
		View v{};
		v.renderView = &views.Get(rv);
		v.eye = eye;
		v.width = 800;
		v.height = 600;
		v.viewProj = views.Get(rv).m_viewProjectionMatrix;
		s.views.push_back(std::move(v));
		s.viewCount = 1;
		PushDraw(s, 0, m, glm::mat4(1.0f), 0);
		BuildBatches(s, 0);
		Check(s.views[0].batchCount == 0, "gfx.cull.away-culls-all");
	}
}

// ---------------------------------------------------------------------------
// T5: two-sided draws skip backface (cone) culling; mirrored instances
// agree with the rasterizer (world-space cone test).
// ---------------------------------------------------------------------------

void TestTwoSided() {
	using namespace smvk::gfx;
	State s{};
	const auto up = MakeSphere(24, 36, 1.0f);
	const uint32_t m = RegisterMeshFromUpload(s, up, 0);
	Check(m == 0, "gfx.twosided.mesh-id");
	const uint32_t clusterTotal = (uint32_t)s.clusters.size();
	Check(clusterTotal > 10, "gfx.twosided.has-clusters");

	smvk::RenderViewSet views;
	views.NewFrame();
	const glm::vec3 eye(0, 0, 4);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f;
	const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });

	// Single-sided draw culls the back hemisphere...
	uint32_t singleKept = 0;
	{
		s.views.clear();
		s.viewCount = 0;
		View v{};
		v.renderView = &views.Get(rv);
		v.eye = eye;
		v.width = 800;
		v.height = 600;
		v.viewProj = views.Get(rv).m_viewProjectionMatrix;
		s.views.push_back(std::move(v));
		s.viewCount = 1;
		PushDraw(s, 0, m, glm::mat4(1.0f), 0);
		BuildBatches(s, 0);
		singleKept = s.views[0].batchCount;
	}
	// ...while a two-sided draw of the same mesh keeps every cluster
	// (the whole sphere is inside the frustum from here).
	{
		s.views.clear();
		s.viewCount = 0;
		View v{};
		v.renderView = &views.Get(rv);
		v.eye = eye;
		v.width = 800;
		v.height = 600;
		v.viewProj = views.Get(rv).m_viewProjectionMatrix;
		s.views.push_back(std::move(v));
		s.viewCount = 1;
		PushDraw(s, 0, m, glm::mat4(1.0f), 0, DrawFlagBit_TwoSided);
		BuildBatches(s, 0);
		const View& out = s.views[0];
#ifndef NDEBUG
		std::cout << "[gfx.twosided] single=" << singleKept
			<< " double=" << out.batchCount << " total=" << clusterTotal << "\n";
#endif
		Check(singleKept < clusterTotal, "gfx.twosided.single-culls");
		Check(out.batchCount == clusterTotal, "gfx.twosided.double-keeps-all");
		Check(out.batchCount > singleKept, "gfx.twosided.double-keeps-more");
		// Flags survive into draw constants (the GPU reads misc.y from these).
		Check(out.drawConstants.size() == 1, "gfx.twosided.one-drawconst");
		Check((out.drawConstants[0].flags & DrawFlagBit_TwoSided) != 0,
			"gfx.twosided.flag-in-drawconst");
	}

	// Mirrored instance: a z-mirror flips the cone axis to face the eye,
	// so a cluster facing away unmirrored must be kept mirrored.
	smvk::RenderViewSet views2;
	views2.NewFrame();
	const glm::vec3 eye2(0, 0, -4);
	const glm::mat4 view2 = glm::lookAt(eye2, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
	glm::mat4 proj2 = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj2[1][1] *= -1.0f;
	const uint32_t rv2 = views2.AllocateMainView(proj2 * view2, { 800.0f, 600.0f });
	const smvk::RenderView& renderView2 = views2.Get(rv2);
	Cluster c{};
	c.boundCenter = glm::vec3(0.0f);
	c.boundRadius = 0.1f;
	c.coneApex = glm::vec3(0.0f);
	c.coneAxis = glm::vec3(0.0f, 0.0f, 1.0f);
	c.coneCutoff = 0.9f;
	const glm::mat4 ident(1.0f);
	const glm::mat4 mirror = glm::scale(ident, glm::vec3(1.0f, 1.0f, -1.0f));
	Check(CullCluster(c, ident, glm::inverse(ident), eye2, renderView2),
		"gfx.twosided.mirror-away-culls");
	Check(!CullCluster(c, mirror, glm::inverse(mirror), eye2, renderView2),
		"gfx.twosided.mirror-keeps");
	Check(!CullCluster(c, ident, glm::inverse(ident), eye2, renderView2, true),
		"gfx.twosided.flag-skips-cone");
}

// ---------------------------------------------------------------------------
// T5 (T1 state): temporal snapshot semantics — pure CPU, headless.
// SnapshotVisible copies this frame's surviving set into the sidecar
// (overwrites = rotation); InvalidateHistory disarms without freeing.
// ---------------------------------------------------------------------------

void TestTemporalSnapshot() {
	using namespace smvk::gfx;
	State s{};
	const auto up = MakeSphere(24, 36, 1.0f);
	const uint32_t m = RegisterMeshFromUpload(s, up, 0);
	Check(m == 0, "gfx.hist.mesh-id");

	smvk::RenderViewSet views;
	views.NewFrame();
	const glm::vec3 eye(0, 0, 4);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f;
	const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });
	View v{};
	v.renderView = &views.Get(rv);
	v.eye = eye;
	v.width = 800;
	v.height = 600;
	v.viewProj = views.Get(rv).m_viewProjectionMatrix;
	s.views.push_back(std::move(v));
	s.viewCount = 1;

	// History starts invalid; invalidating fresh state is a safe no-op.
	Check(!Temporal(s, 0).historyValid, "gfx.hist.starts-invalid");
	InvalidateHistory(s, 0);
	Check(!Temporal(s, 0).historyValid, "gfx.hist.invalidate-noop");

	PushDraw(s, 0, m, glm::mat4(1.0f), 0);
	BuildBatches(s, 0);
	const uint32_t kept = s.views[0].batchCount;
	Check(kept > 0, "gfx.hist.kept-some");

	SnapshotVisible(s, 0);
	const TemporalView& t = Temporal(s, 0);
	Check(t.historyValid, "gfx.hist.armed");
	Check(t.prevBatchCount == kept, "gfx.hist.batch-count");
	Check(t.prevDrawCount == (uint32_t)s.views[0].drawConstants.size(),
		"gfx.hist.draw-count");
	Check(t.prevDraws.size() == s.views[0].drawConstants.size(),
		"gfx.hist.draws-cover");
	bool pairsOk = t.prevBatches.size() == s.views[0].batches.size();
	for (size_t i = 0; pairsOk && i < t.prevBatches.size(); ++i) {
		pairsOk = t.prevBatches[i].cluster == s.views[0].batches[i].cluster
			&& t.prevBatches[i].draw == s.views[0].batches[i].draw;
	}
	Check(pairsOk, "gfx.hist.pairs-match");
	Check(t.prevRanges.size() == s.views[0].materialRanges.size(),
		"gfx.hist.ranges-cover");
	Check(t.width == 800 && t.height == 600, "gfx.hist.extent");

	// Rotation overwrites, never appends (simulate an empty follow-up frame).
	s.views[0].batches.clear();
	s.views[0].drawConstants.clear();
	s.views[0].materialRanges.clear();
	s.views[0].batchCount = 0;
	SnapshotVisible(s, 0);
	const TemporalView& t2 = Temporal(s, 0);
	Check(t2.prevBatchCount == 0 && t2.prevBatches.empty()
		&& t2.prevDraws.empty() && t2.prevRanges.empty(),
		"gfx.hist.rotate-overwrites");

	// Invalidate disarms but keeps buffers (next frame rebuilds).
	InvalidateHistory(s, 0);
	Check(!Temporal(s, 0).historyValid, "gfx.hist.invalidate-disarms");
}

// ---------------------------------------------------------------------------
// T6 (T2 state): HZB mip layout — pure CPU, headless. Guards the manual
// mip-offset math shared (by convention) with hzb.cs.hlsl addressing.
// ---------------------------------------------------------------------------

void TestHizLayout() {
	using namespace smvk::gfx;
	Check(HzbMipCount(0, 0) == 0, "gfx.hiz.zero-extent");
	Check(HzbMipCount(1, 1) == 1, "gfx.hiz.single-texel");
	Check(HzbMipCount(64, 64) == 7, "gfx.hiz.pow2-levels");
	Check(HzbMipCount(1920, 1080) == 12, "gfx.hiz.hd-levels");

	uint32_t w = 0, h = 0;
	HzbMipDims(1920, 1080, 0, w, h);
	Check(w == 1920 && h == 1080, "gfx.hiz.dims-mip0");
	HzbMipDims(1920, 1080, 1, w, h);
	Check(w == 960 && h == 540, "gfx.hiz.dims-mip1");
	HzbMipDims(1920, 1080, 11, w, h);
	Check(w == 1 && h == 1, "gfx.hiz.dims-last");
	// Odd-extent regression (ceil-halve): the builder's clamped 2x2 folds
	// the odd edge into the last texel, so 240x135 -> 120x68 (not 120x67).
	HzbMipDims(240, 135, 1, w, h);
	Check(w == 120 && h == 68, "gfx.hiz.dims-odd-ceil");
	HzbMipDims(5, 3, 1, w, h);
	Check(w == 3 && h == 2, "gfx.hiz.dims-5x3-mip1");
	HzbMipDims(5, 3, 2, w, h);
	Check(w == 2 && h == 1, "gfx.hiz.dims-5x3-mip2");
	HzbMipDims(5, 3, 3, w, h);
	Check(w == 1 && h == 1, "gfx.hiz.dims-5x3-mip3");
	Check(HzbMipOffsetFloats(5, 3, 1) == 5u * 3u, "gfx.hiz.off-odd-mip1");
	Check(HzbMipOffsetFloats(5, 3, 2) == 5u * 3u + 3u * 2u, "gfx.hiz.off-odd-mip2");

	Check(HzbMipOffsetFloats(1920, 1080, 0) == 0, "gfx.hiz.off-mip0");
	Check(HzbMipOffsetFloats(1920, 1080, 1) == 1920u * 1080u, "gfx.hiz.off-mip1");
	Check(HzbMipOffsetFloats(1920, 1080, 2) == 1920u * 1080u + 960u * 540u,
		"gfx.hiz.off-mip2");
	// 64x64: 4096+1024+256+64+16+4+1.
	Check(HzbTotalFloats(64, 64) == 5461, "gfx.hiz.total-pow2");
	Check(HzbTotalFloats(0, 5) == 0, "gfx.hiz.total-zero");
	// Total covers the last mip exactly (no gap, no overrun).
	{
		const uint32_t total = HzbTotalFloats(1920, 1080);
		uint32_t lw = 0, lh = 0;
		HzbMipDims(1920, 1080, HzbMipCount(1920, 1080) - 1, lw, lh);
		Check(total == HzbMipOffsetFloats(1920, 1080, 11) + lw * lh,
			"gfx.hiz.total-tight");
	}

	// Reversed-Z conservative reduction: MIN (sky 0.0 poisons to keep).
	Check(HzbReduce2x2(0.9f, 0.85f, 0.92f, 0.88f) == 0.85f, "gfx.hiz.reduce-min");
	Check(HzbReduce2x2(0.0f, 0.9f, 0.8f, 0.7f) == 0.0f, "gfx.hiz.reduce-sky");
}

// ---------------------------------------------------------------------------
// T6 (T3 state): seed command build — bucket-major reorder with
// per-(bucket, material) ranges, drawn from the snapshot (pure CPU).
// ---------------------------------------------------------------------------

void TestSeedCommands() {
	using namespace smvk::gfx;
	State s{};
	const uint32_t m0 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)), 0);
	const uint32_t m1 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(2, 0, 0), glm::vec3(3, 0, 0), glm::vec3(2, 1, 0)), 3);
	Check(m0 == 0 && m1 == 1, "gfx.seed.mesh-ids");

	smvk::RenderViewSet views;
	views.NewFrame();
	const glm::vec3 eye(1.5f, 0.5f, 5.0f);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(1.5f, 0.5f, 0.0f), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f;
	const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });
	View v{};
	v.renderView = &views.Get(rv);
	v.eye = eye;
	v.width = 800;
	v.height = 600;
	v.viewProj = views.Get(rv).m_viewProjectionMatrix;
	s.views.push_back(std::move(v));
	s.viewCount = 1;

	// No history -> no seed commands (ranges cleared too).
	BuildSeedCommands(s, 0);
	Check(Temporal(s, 0).seedCommands.empty(), "gfx.seed.no-history-empty");

	// Mixed buckets, shuffled materials: opaque must come first.
	PushDraw(s, 0, m1, glm::mat4(1.0f), 7, 0, DrawSetBit_Masked);
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, DrawFlagBit_TwoSided, DrawSetBit_Opaque);
	BuildBatches(s, 0);
	Check(s.views[0].batchCount == 3, "gfx.seed.three-batches");
	SnapshotVisible(s, 0);
	BuildSeedCommands(s, 0);
	const TemporalView& t = Temporal(s, 0);
	Check(t.seedCommands.size() == 3, "gfx.seed.cmd-count");
	Check(t.seedOpaqueCommands == 2, "gfx.seed.opaque-first");
	Check(t.seedRanges.size() == 2, "gfx.seed.two-ranges");
	bool rangesOk = false;
	if (t.seedRanges.size() == 2) {
		const SeedRange& o = t.seedRanges[0];
		const SeedRange& mk = t.seedRanges[1];
		rangesOk = o.drawSet == DrawSetBit_Opaque && o.material == 3
			&& o.firstCommand == 0 && o.commandCount == 2
			&& mk.drawSet == DrawSetBit_Masked && mk.material == 7
			&& mk.firstCommand == 2 && mk.commandCount == 1;
	}
	Check(rangesOk, "gfx.seed.ranges-bucketed");
	// Commands reference the cluster-ordered heap + snapshot draws.
	// Clusters: m0 -> 0, m1 -> 1 (single-triangle meshes). Batch.draw is
	// the material-sorted draw index: mat3 draws sort first, so the
	// snapshot draws are [m0 opaque, m0 opaque, m1 masked] and the
	// bucket-major commands are (c0,d0) (c0,d1) (c1,d2).
	bool cmdsOk = t.seedCommands.size() == 3;
	const uint32_t expectCluster[3] = { 0, 0, 1 };
	const uint32_t expectDraw[3] = { 0, 1, 2 };
	for (size_t i = 0; cmdsOk && i < 3; ++i) {
		const SeedCommand& c = t.seedCommands[i];
		const Cluster& cl = s.clusters[expectCluster[i]];
		cmdsOk = c.instanceCount == 1 && c.vertexOffset == 0
			&& c.indexCount == cl.triCount * 3
			&& c.firstIndex == cl.indexOffset
			&& c.firstInstance == expectDraw[i];
	}
	Check(cmdsOk, "gfx.seed.cmds-shape");
	// firstIndex is absolute into the static index monoblock (region base
	// + cluster offset), so the IA index binding never switches buffers.
	// Headless State has no GPU region (base 0); pin the formula with a
	// nonzero sentinel base.
	s.clusterIndexBase = 1337u;
	BuildSeedCommands(s, 0);
	bool absOk = Temporal(s, 0).seedCommands.size() == 3;
	for (size_t i = 0; absOk && i < 3; ++i) {
		const SeedCommand& c = Temporal(s, 0).seedCommands[i];
		absOk = c.firstIndex
			== 1337u + s.clusters[expectCluster[i]].indexOffset;
	}
	Check(absOk, "gfx.seed.abs-firstindex");
	s.clusterIndexBase = 0;

	// Invalidating clears the seed list (no stale commands survive).
	InvalidateHistory(s, 0);
	BuildSeedCommands(s, 0);
	Check(Temporal(s, 0).seedCommands.empty()
		&& Temporal(s, 0).seedRanges.empty(),
		"gfx.seed.invalidate-clears");
}

// ---------------------------------------------------------------------------
// Seed/current intersection: the seed may only replay prev batches whose
// (cluster, model) survives into this frame's batches. A stale prev-LOD
// sibling or moved instance would otherwise replay nearer depth that the
// read-only Main (GreaterEqual over Load'ed seed + full prepass) fails
// against — one frame of skybox holes on every LOD flip. Pure CPU.
// ---------------------------------------------------------------------------

void TestSeedIntersection() {
	using namespace smvk::gfx;
	State s{};
	const uint32_t m0 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)), 0);
	const uint32_t m1 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(2, 0, 0), glm::vec3(3, 0, 0), glm::vec3(2, 1, 0)), 3);
	const uint32_t m2 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(4, 0, 0), glm::vec3(5, 0, 0), glm::vec3(4, 1, 0)), 6);
	Check(m0 == 0 && m1 == 1 && m2 == 2, "gfx.seedix.mesh-ids");

	smvk::RenderViewSet views;
	views.NewFrame();
	const glm::vec3 eye(1.5f, 0.5f, 5.0f);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(1.5f, 0.5f, 0.0f), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f;
	const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });
	auto freshView = [&]() {
		s.views.clear();
		s.viewCount = 0;
		View v{};
		v.renderView = &views.Get(rv);
		v.eye = eye;
		v.width = 800;
		v.height = 600;
		v.viewProj = views.Get(rv).m_viewProjectionMatrix;
		s.views.push_back(std::move(v));
		s.viewCount = 1;
	};

	// Frame A baseline: m0 (opaque mat3) + m1 (opaque mat5).
	freshView();
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 5, 0, DrawSetBit_Opaque);
	BuildBatches(s, 0);
	Check(s.views[0].batchCount == 2, "gfx.seedix.frame-a-batches");
	SnapshotVisible(s, 0);

	// Frame B identical: the whole snapshot is still current -> full seed.
	freshView();
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 5, 0, DrawSetBit_Opaque);
	BuildBatches(s, 0);
	BuildSeedCommands(s, 0);
	Check(Temporal(s, 0).seedCommands.size() == 2, "gfx.seedix.stable-full-seed");

	// Frame C (LOD flip): m0's slot now draws sibling m2 (disjoint cluster
	// ids), m1 unchanged. Seed must carry only m1's prev batch — replaying
	// m0's stale cluster would poison depth Main tests read-only.
	freshView();
	PushDraw(s, 0, m2, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 5, 0, DrawSetBit_Opaque);
	BuildBatches(s, 0);
	BuildSeedCommands(s, 0);
	bool flipOk = false;
	if (Temporal(s, 0).seedCommands.size() == 1) {
		const SeedCommand& c = Temporal(s, 0).seedCommands[0];
		// Prev snapshot draws sorted mat3 first: [m0, m1], so m1 == draw 1.
		flipOk = c.firstIndex == s.clusters[1].indexOffset
			&& c.firstInstance == 1
			&& c.indexCount == 3;
	}
	Check(flipOk, "gfx.seedix.lod-flip-seeds-survivor-only");

	// Frame D (moved instance): m0 translated, m1 static. Only m1 seeds.
	freshView();
	glm::mat4 moved(1.0f);
	moved[3][0] = 0.5f;
	PushDraw(s, 0, m0, moved, 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 5, 0, DrawSetBit_Opaque);
	BuildBatches(s, 0);
	BuildSeedCommands(s, 0);
	bool movedOk = false;
	if (Temporal(s, 0).seedCommands.size() == 1) {
		const SeedCommand& c = Temporal(s, 0).seedCommands[0];
		movedOk = c.firstIndex == s.clusters[1].indexOffset
			&& c.firstInstance == 1;
	}
	Check(movedOk, "gfx.seedix.moved-seeds-static-only");
}

// ---------------------------------------------------------------------------
// T6 (T4 state): HZB occlusion mirror — pure CPU, headless. Pins the
// projection / mip-select / compare math the CS duplicates in HLSL.
// ---------------------------------------------------------------------------

void TestHzbOcclusion() {
	using namespace smvk::gfx;
	const glm::vec3 eye(0, 0, 4);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f; // engine Y-flip, mirrors UpdateFrameUniforms
	const glm::mat4 viewProj = proj * view;
	constexpr uint32_t W = 800, H = 600;

	// Frontal unit sphere: centered, radii in the hundreds of px.
	ClusterFootprint f = ProjectClusterFootprint(
		glm::vec3(0, 0, 0), 1.0f, eye, viewProj, W, H);
	Check(!f.behindCamera, "gfx.occ.front-testable");
	Check(std::abs(f.centerX - 400.0f) < 1.0f && std::abs(f.centerY - 300.0f) < 1.0f,
		"gfx.occ.front-center");
	// Radii use nearest-point depth (conservative overestimate vs radial).
	Check(f.radiusX > 190.0f && f.radiusX < 280.0f
		&& f.radiusY > 140.0f && f.radiusY < 215.0f, "gfx.occ.front-radii");
	Check(f.nearestDepth > 0.0f && f.nearestDepth < 1.0f, "gfx.occ.depth-range");
	{
		// Nearer point = larger reversed-Z value than the center.
		const glm::vec4 clipC = viewProj * glm::vec4(0, 0, 0, 1);
		const float dCenter = clipC.z / clipC.w;
		Check(f.nearestDepth > dCenter, "gfx.occ.near-beats-center");
	}

	// Behind the projection's camera / eye-inside keep (no test possible).
	Check(ProjectClusterFootprint(glm::vec3(0, 0, 8), 1.0f, eye,
		viewProj, W, H).behindCamera, "gfx.occ.behind-keeps");
	Check(ProjectClusterFootprint(glm::vec3(0, 0, 3.5f), 1.0f, eye,
		viewProj, W, H).behindCamera, "gfx.occ.inside-keeps");

	// Mip selection: smallest texel covering the rect.
	Check(HzbMipForRect(1.0f, 1.0f, 11) == 0, "gfx.occ.mip-subtexel");
	Check(HzbMipForRect(300.0f, 200.0f, 11) == 9, "gfx.occ.mip-rect");
	Check(HzbMipForRect(5000.0f, 10.0f, 11) == 10, "gfx.occ.mip-clamp");

	// Occlusion over an explicit pyramid (8x8 uniform 0.9 -> mips all 0.9).
	std::vector<float> hzb(HzbTotalFloats(8, 8), 0.9f);
	const uint32_t mips = HzbMipCount(8, 8);
	Check(mips == 4, "gfx.hiz.tiny-levels");
	ClusterFootprint vis{};
	vis.behindCamera = false;
	vis.centerX = 4.0f; vis.centerY = 4.0f;
	vis.radiusX = 50.0f; vis.radiusY = 50.0f; // clamped mip, center texel
	vis.nearestDepth = 0.5f; // behind the 0.9 surface
	Check(HzbOccluded(hzb.data(), 8, 8, mips, vis), "gfx.occ.hidden");
	vis.nearestDepth = 0.95f; // in front: keep
	Check(!HzbOccluded(hzb.data(), 8, 8, mips, vis), "gfx.occ.visible-keeps");
	// Sky-poisoned texel under a tiny rect: keep (conservative).
	hzb[HzbMipOffsetFloats(8, 8, 0) + 4 * 8 + 4] = 0.0f;
	ClusterFootprint tiny = vis;
	tiny.radiusX = tiny.radiusY = 0.25f;
	tiny.nearestDepth = 0.5f;
	Check(!HzbOccluded(hzb.data(), 8, 8, mips, tiny), "gfx.occ.sky-keeps");
	// Null/empty pyramid keeps unconditionally.
	Check(!HzbOccluded(nullptr, 8, 8, mips, vis), "gfx.occ.null-keeps");
	Check(!HzbOccluded(hzb.data(), 0, 0, 0, vis), "gfx.occ.empty-keeps");
	// Fully off-target rect belongs to the frustum stages, not occlusion.
	ClusterFootprint off = vis;
	off.centerX = 5000.0f;
	Check(!HzbOccluded(hzb.data(), 8, 8, mips, off), "gfx.occ.offscreen-keeps");

	// Y-row discrimination: world +Y (up) must map to the TOP half of the
	// target (rows y-down) under the engine Y-flip. Catches a row-mirror
	// (wrong-texel) regression that mid-screen tests cannot see.
	{
		ClusterFootprint up = ProjectClusterFootprint(
			glm::vec3(0, 1, 0), 0.25f, eye, viewProj, W, H);
		Check(!up.behindCamera, "gfx.occ.up-testable");
		Check(up.centerY < (float)H * 0.5f, "gfx.occ.up-is-top");
		ClusterFootprint dn = ProjectClusterFootprint(
			glm::vec3(0, -1, 0), 0.25f, eye, viewProj, W, H);
		Check(!dn.behindCamera, "gfx.occ.down-testable");
		Check(dn.centerY > (float)H * 0.5f, "gfx.occ.down-is-bottom");
	}
	// Row-index discrimination: only the addressed row occludes. A Y-mirror
	// in fetch (vs projection) would read row H-1-y and keep here.
	{
		const uint32_t W8 = 8, H8 = 8;
		const uint32_t m8 = HzbMipCount(W8, H8);
		std::vector<float> rows(HzbTotalFloats(W8, H8), 0.0f);
		rows[HzbMipOffsetFloats(W8, H8, 0) + 2 * W8 + 4] = 0.9f; // row 2 only
		ClusterFootprint row{};
		row.behindCamera = false;
		row.centerX = 4.5f; row.centerY = 2.5f; // texel interior: single texel
		row.radiusX = row.radiusY = 0.25f;
		row.nearestDepth = 0.5f;
		Check(HzbOccluded(rows.data(), W8, H8, m8, row), "gfx.occ.row-hit");
		ClusterFootprint rowMiss = row;
		rowMiss.centerY = 5.5f; // row 5 is far (0.0): keep
		Check(!HzbOccluded(rows.data(), W8, H8, m8, rowMiss), "gfx.occ.row-miss-keeps");
	}
	// Cover conservatism: rect [5,11]^2 at level 3 (texel 8, 2x2) spans
	// all four texels. Only texel (1,1) near -> keep (edges see far).
	// All four near -> hidden.
	{
		const uint32_t W16 = 16, H16 = 16;
		const uint32_t m16 = HzbMipCount(W16, H16);
		Check(HzbMipForRect(6.0f, 6.0f, m16) == 3, "gfx.occ.cover-level");
		ClusterFootprint wide{};
		wide.behindCamera = false;
		wide.centerX = 8.0f; wide.centerY = 8.0f;
		wide.radiusX = wide.radiusY = 3.0f;
		wide.nearestDepth = 0.5f;
		std::vector<float> het(HzbTotalFloats(W16, H16), 0.0f);
		het[HzbMipOffsetFloats(W16, H16, 3) + 1 * 2 + 1] = 0.9f;
		Check(!HzbOccluded(het.data(), W16, H16, m16, wide), "gfx.occ.cover-keeps");
		for (uint32_t k = 0; k < 4; ++k)
			het[HzbMipOffsetFloats(W16, H16, 3) + k] = 0.9f;
		Check(HzbOccluded(het.data(), W16, H16, m16, wide), "gfx.occ.cover-hidden");
	}
}

// ---------------------------------------------------------------------------
// T6 (T5 state): remainder difference — currVisible minus prevVisible keyed
// by (cluster, model matrix). Pure CPU, headless.
// ---------------------------------------------------------------------------

void TestRemainderCommands() {
	using namespace smvk::gfx;
	State s{};
	const uint32_t m0 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)), 0);
	const uint32_t m1 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(2, 0, 0), glm::vec3(3, 0, 0), glm::vec3(2, 1, 0)), 3);
	Check(m0 == 0 && m1 == 1, "gfx.rem.mesh-ids");

	smvk::RenderViewSet views;
	views.NewFrame();
	const glm::vec3 eye(1.5f, 0.5f, 5.0f);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(1.5f, 0.5f, 0.0f), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f;
	const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });
	auto freshView = [&]() {
		s.views.clear();
		s.viewCount = 0;
		View v{};
		v.renderView = &views.Get(rv);
		v.eye = eye;
		v.width = 800;
		v.height = 600;
		v.viewProj = views.Get(rv).m_viewProjectionMatrix;
		s.views.push_back(std::move(v));
		s.viewCount = 1;
	};

	// Frame A: two draws, snapshot as baseline.
	freshView();
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 7, 0, DrawSetBit_Masked);
	BuildBatches(s, 0);
	Check(s.views[0].batchCount == 2, "gfx.rem.frame-a-batches");
	SnapshotVisible(s, 0);

	// Frame B identical: nothing newly visible.
	freshView();
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 7, 0, DrawSetBit_Masked);
	BuildBatches(s, 0);
	BuildRemainderCommands(s, 0);
	Check(Temporal(s, 0).remainderCommands.empty()
		&& Temporal(s, 0).remainderRanges.empty(),
		"gfx.rem.static-empty");

	// Frame C: m0 translated (moved instance) -> only its cluster is new.
	// m1 untouched -> covered by the snapshot.
	freshView();
	glm::mat4 moved(1.0f);
	moved[3][0] = 0.5f;
	PushDraw(s, 0, m0, moved, 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 7, 0, DrawSetBit_Masked);
	BuildBatches(s, 0);
	BuildRemainderCommands(s, 0);
	const TemporalView& t = Temporal(s, 0);
	Check(t.remainderCommands.size() == 1, "gfx.rem.moved-count");
	Check(t.remainderOpaqueCommands == 1, "gfx.rem.moved-opaque");
	bool remOk = false;
	if (t.remainderCommands.size() == 1 && t.remainderRanges.size() == 1) {
		const SeedCommand& c = t.remainderCommands[0];
		const SeedRange& r = t.remainderRanges[0];
		// Draw order sorts mat3 first: curr draw 0 = moved m0.
		remOk = c.firstInstance == 0
			&& c.firstIndex == s.clusters[0].indexOffset
			&& c.indexCount == 3
			&& r.drawSet == DrawSetBit_Opaque && r.material == 3
			&& r.firstCommand == 0 && r.commandCount == 1;
	}
	Check(remOk, "gfx.rem.moved-shape");

	// No baseline (invalidated): empty even though geometry is "new" —
	// the full prepass covered it, so pass 2 has nothing to add.
	InvalidateHistory(s, 0);
	BuildRemainderCommands(s, 0);
	Check(Temporal(s, 0).remainderCommands.empty(), "gfx.rem.invalid-empty");
}

// ---------------------------------------------------------------------------
// Sorted-key merge machinery (detail::MakeVisKey / detail::MergeKeyMasks):
// the seed/remainder split reduces (cluster, model) to u64 keys. Hash ties
// must verify with a real memcmp or a collision would seed stale depth —
// the exact bug the intersection exists to prevent. Drives the merge
// with every key tied (worst-case collisions) plus a distinct-key pass.
// Pure CPU.
// ---------------------------------------------------------------------------

void TestVisKeyMerge() {
	using namespace smvk::gfx;
	const glm::mat4 A(1.0f);
	glm::mat4 B(1.0f);
	B[3][0] = 0.5f;

	// MakeVisKey: same bits -> same key; moved -> different; -0.0 vs
	// +0.0 stays distinct (bitwise, matching the memcmp identity); the
	// cluster id rides the low bits.
	glm::mat4 negZero(1.0f);
	negZero[0][0] = -0.0f;
	Check(detail::MakeVisKey(A, 0) == detail::MakeVisKey(A, 0), "gfx.viskey.stable");
	Check(detail::MakeVisKey(A, 0) != detail::MakeVisKey(B, 0), "gfx.viskey.moved-distinct");
	Check(detail::MakeVisKey(A, 0) != detail::MakeVisKey(negZero, 0), "gfx.viskey.negzero-distinct");
	Check(detail::MakeVisKey(A, 0) != detail::MakeVisKey(A, 1), "gfx.viskey.cluster-distinct");

	// Worst case: every key collides (as if all model hashes tied). Prev
	// = A, B, A (batches 0,1,2); curr = B, A (batches 0,1). Tie-group
	// verification must match across models: A's find A's, B finds B.
	{
		const glm::mat4 prevModels[3] = { A, B, A };
		const glm::mat4 currModels[2] = { B, A };
		const auto modelEq = [&](uint32_t p, uint32_t c) {
			return std::memcmp(&prevModels[p], &currModels[c], sizeof(glm::mat4)) == 0;
		};
		const detail::VisKeyEntry tiedPrev[3] = { {1, 0}, {1, 1}, {1, 2} };
		const detail::VisKeyEntry tiedCurr[2] = { {1, 0}, {1, 1} };
		uint8_t seed[3] = {}, rem[2] = {};
		detail::MergeKeyMasks(tiedPrev, 3, tiedCurr, 2, modelEq, seed, rem);
		Check(seed[0] == 1 && seed[1] == 1 && seed[2] == 1, "gfx.viskey.tied-all-match");
		Check(rem[0] == 0 && rem[1] == 0, "gfx.viskey.tied-no-remainder");
	}

	// Same key, different model bits: the verify must REJECT (collision
	// cannot seed stale depth) and the curr entry must fall to remainder.
	{
		const glm::mat4 prevModels[1] = { A };
		const glm::mat4 currModels[1] = { B };
		const auto modelEq = [&](uint32_t p, uint32_t c) {
			return std::memcmp(&prevModels[p], &currModels[c], sizeof(glm::mat4)) == 0;
		};
		const detail::VisKeyEntry collidePrev[1] = { {7, 0} };
		const detail::VisKeyEntry collideCurr[1] = { {7, 0} };
		uint8_t seed[1] = {}, rem[1] = {};
		detail::MergeKeyMasks(collidePrev, 1, collideCurr, 1, modelEq, seed, rem);
		Check(seed[0] == 0, "gfx.viskey.collision-rejects");
		Check(rem[0] == 1, "gfx.viskey.collision-remainders");
	}

	// Distinct keys (sorted fast paths + one tie with equal models):
	// prev = keys {1, 3} -> A, B; curr = keys {2, 3, 5} -> B, B, A. Only
	// the key-3 pair matches; everything else lands on its side of the
	// difference (curr key 2 and key 5 -> remainder, prev key 1 -> no seed).
	{
		const glm::mat4 prevModels[2] = { A, B };
		const glm::mat4 currModels[3] = { B, B, A };
		const auto modelEq = [&](uint32_t p, uint32_t c) {
			return std::memcmp(&prevModels[p], &currModels[c], sizeof(glm::mat4)) == 0;
		};
		const detail::VisKeyEntry distinctPrev[2] = { {1, 0}, {3, 1} };
		const detail::VisKeyEntry distinctCurr[3] = { {2, 0}, {3, 1}, {5, 2} };
		uint8_t seed[2] = {}, rem[3] = {};
		detail::MergeKeyMasks(distinctPrev, 2, distinctCurr, 3, modelEq, seed, rem);
		Check(seed[0] == 0 && seed[1] == 1, "gfx.viskey.sorted-merge-seed");
		Check(rem[0] == 1 && rem[1] == 0 && rem[2] == 1, "gfx.viskey.sorted-merge-remainder");
	}
}

// ---------------------------------------------------------------------------
// Multiset semantics end-to-end: identical co-located instances produce
// duplicate (cluster, model) keys; BOTH must seed (the pre-merge
// binary_search kept them), the fused builder must produce the same
// coalesced ranges, and SnapshotVisible's key rotation (memo path) must
// keep the next frame's full seed intact. Pure CPU.
// ---------------------------------------------------------------------------

void TestSeedDuplicateBatches() {
	using namespace smvk::gfx;
	State s{};
	const uint32_t m0 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)), 0);
	const uint32_t m1 = RegisterMeshFromUpload(s, MakeTri(
		glm::vec3(2, 0, 0), glm::vec3(3, 0, 0), glm::vec3(2, 1, 0)), 3);
	Check(m0 == 0 && m1 == 1, "gfx.seeddup.mesh-ids");

	smvk::RenderViewSet views;
	views.NewFrame();
	const glm::vec3 eye(1.5f, 0.5f, 5.0f);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(1.5f, 0.5f, 0.0f), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f;
	const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });
	auto freshView = [&]() {
		s.views.clear();
		s.viewCount = 0;
		View v{};
		v.renderView = &views.Get(rv);
		v.eye = eye;
		v.width = 800;
		v.height = 600;
		v.viewProj = views.Get(rv).m_viewProjectionMatrix;
		s.views.push_back(std::move(v));
		s.viewCount = 1;
	};

	// Frame A: m0 drawn twice with the SAME transform (identical co-located
	// instances -> duplicate keys) + m1.
	freshView();
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	BuildBatches(s, 0);
	Check(s.views[0].batchCount == 3, "gfx.seeddup.frame-a-batches");
	SnapshotVisible(s, 0);

	// Frame B identical: both duplicates must seed, remainder empty, one
	// material -> one coalesced range over all three commands.
	freshView();
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	BuildBatches(s, 0);
	BuildSeedAndRemainder(s, 0);
	const TemporalView& t = Temporal(s, 0);
	Check(t.seedCommands.size() == 3, "gfx.seeddup.dup-seeds-both");
	Check(t.seedOpaqueCommands == 3, "gfx.seeddup.all-opaque");
	Check(t.remainderCommands.empty() && t.remainderRanges.empty(),
		"gfx.seeddup.remainder-empty");
	bool rangeOk = t.seedRanges.size() == 1;
	if (rangeOk) {
		const SeedRange& r = t.seedRanges[0];
		rangeOk = r.drawSet == DrawSetBit_Opaque && r.material == 3
			&& r.firstCommand == 0 && r.commandCount == 3;
	}
	Check(rangeOk, "gfx.seeddup.range-coalesced");

	// Frame B -> C rotation through the memo path: SnapshotVisible reuses
	// the builder's sorted curr keys instead of re-sorting. Frame C must
	// still full-seed from the rotated keys.
	SnapshotVisible(s, 0);
	freshView();
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m0, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	PushDraw(s, 0, m1, glm::mat4(1.0f), 3, 0, DrawSetBit_Opaque);
	BuildBatches(s, 0);
	BuildSeedAndRemainder(s, 0);
	Check(Temporal(s, 0).seedCommands.size() == 3,
		"gfx.seeddup.key-rotate-full-seed");
}

// ---------------------------------------------------------------------------
// T6 (spawn-view repro): reversed-Z HZB end-to-end over a synthetic render of
// the sponza spawn framing (cloth banners center screen, back wall behind).
// Software-rasterizes depth exactly like the engine (reversed-Z, clear 0,
// nearest-wins), builds the pyramid with the hzb.cs.hlsl MIN mirror, then
// runs the CPU occlusion mirror over flat cloth patches. The cloth must
// survive its own depth pyramid (the bounding-sphere pole lands r in FRONT
// of the surface -> dNear > stored by construction); a wall patch behind it
// must cull (harness validity: the test can see both directions).
// ---------------------------------------------------------------------------

void TestHzbSpawnView() {
	using namespace smvk::gfx;
	constexpr uint32_t W = 480, H = 270;
	const float nearP = 0.01f;
	const glm::vec3 eye(0.0f, 1.0f, 0.0f);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(0, 1, 1), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(45.0f), W / float(H), nearP);
	proj[1][1] *= -1.0f; // engine Y-flip (UpdateFrameUniforms)
	const glm::mat4 viewProj = proj * view;
	const glm::mat4 invViewProj = glm::inverse(viewProj);

	// Cloth banner (center screen, double-sided) + back wall behind it.
	const glm::vec3 clothA(-0.5f, 0.6f, 2.0f), clothB(0.5f, 2.0f, 2.0f);
	const glm::vec3 wallA(-4.0f, 0.0f, 5.0f), wallB(4.0f, 4.0f, 5.0f);

	// Software raster: per-pixel nearest hit -> reversed-Z depth = n / |z_view|.
	std::vector<float> depth((size_t)W * H, 0.0f); // sky = 0 (reversed-Z clear)
	for (uint32_t y = 0; y < H; ++y) {
		for (uint32_t x = 0; x < W; ++x) {
			const glm::vec2 ndc((float(x) + 0.5f) / W * 2.0f - 1.0f,
				(float(y) + 0.5f) / H * 2.0f - 1.0f);
		// NDC z: reversed-Z maps near=1; unproject near corner + direction.
		// Infinite far plane: the far unproject lands at/beyond infinity
		// (f4.w ~= 0), so a raw vec3(f4)/f4.w division yields inf/NaN for
		// near-center pixels. A point at infinity IS a direction: fall back
		// to normalize(vec3(f4)) when |w| is degenerate.
		const glm::vec4 n4 = invViewProj * glm::vec4(ndc, 1.0f, 1.0f);
		const glm::vec4 f4 = invViewProj * glm::vec4(ndc, 0.0f, 1.0f);
		const glm::vec3 o = glm::vec3(n4) / n4.w;
		const glm::vec3 d = (std::abs(f4.w) > 1e-9f)
			? glm::normalize(glm::vec3(f4) / f4.w - o)
			: glm::normalize(glm::vec3(f4));
			float bestZ = -1.0f; // |z_view| of the nearest hit
			auto quad = [&](float planeZ, const glm::vec3& a, const glm::vec3& b) {
				if (std::abs(d.z) < 1e-9f) return;
				const float t = (planeZ - o.z) / d.z;
				if (t < 1e-6f) return;
				const glm::vec3 p = o + d * t;
				if (p.x < a.x || p.x > b.x || p.y < a.y || p.y > b.y) return;
				const float z = std::abs((view * glm::vec4(p, 1.0f)).z);
				if (bestZ < 0.0f || z < bestZ) bestZ = z;
			};
			quad(2.0f, clothA, clothB);
			quad(5.0f, wallA, wallB);
			if (bestZ > 0.0f)
				depth[(size_t)y * W + x] = nearP / bestZ;
		}
	}
	// Sanity: the center pixel hits the cloth interior (not sky, not wall).
	const float centerDepth = depth[(size_t)(H / 2) * W + W / 2];
	const float clothDepth = nearP / std::abs((view * glm::vec4(0.0f, 1.0f, 2.0f, 1.0f)).z);
	Check(std::abs(centerDepth - clothDepth) < clothDepth * 0.05f,
		"gfx.spawn.center-is-cloth");

	// HZB build: exact hzb.cs.hlsl mirror (mip0 copy, MIN 2x2 clamped).
	const uint32_t mips = HzbMipCount(W, H);
	std::vector<float> hzb(HzbTotalFloats(W, H), 0.0f);
	std::copy(depth.begin(), depth.end(), hzb.begin());
	for (uint32_t m = 1; m < mips; ++m) {
		uint32_t sw = 0, sh = 0, dw = 0, dh = 0;
		HzbMipDims(W, H, m - 1, sw, sh);
		HzbMipDims(W, H, m, dw, dh);
		const uint32_t srcOff = HzbMipOffsetFloats(W, H, m - 1);
		const uint32_t dstOff = HzbMipOffsetFloats(W, H, m);
		for (uint32_t y = 0; y < dh; ++y) {
			for (uint32_t x = 0; x < dw; ++x) {
				const uint32_t bx = x * 2, by = y * 2;
				const uint32_t sx1 = std::min(bx + 1, sw - 1);
				const uint32_t sy1 = std::min(by + 1, sh - 1);
				hzb[dstOff + y * dw + x] = HzbReduce2x2(
					hzb[srcOff + by * sw + bx], hzb[srcOff + by * sw + sx1],
					hzb[srcOff + sy1 * sw + bx], hzb[srcOff + sy1 * sw + sx1]);
			}
		}
	}

	// Cloth patches (4x4 flat grid): every one must survive its own pyramid.
	bool allKeep = true;
	ClusterFootprint centerF{};
	float bestCenterDist = std::numeric_limits<float>::max();
	for (uint32_t iy = 0; iy < 4; ++iy) {
		for (uint32_t ix = 0; ix < 4; ++ix) {
			const glm::vec3 c(clothA.x + (ix + 0.5f) * 0.25f,
				clothA.y + (iy + 0.5f) * 0.35f, 2.0f);
			const float r = 0.5f * std::sqrt(0.25f * 0.25f + 0.35f * 0.35f);
			ClusterFootprint f = ProjectClusterFootprint(c, r, eye, viewProj, W, H);
			if (f.behindCamera) { allKeep = false; continue; }
			float stored = -1.0f;
			const bool occ = HzbOccluded(hzb.data(), W, H, mips, f, &stored);
			if (occ) {
				allKeep = false;
				SMVK_DEBUG_LOG("[gfx.spawn] cloth patch (%u,%u) CULLED: dNear=%.6f stored=%.6f "
					"c=(%.1f,%.1f) r=(%.1f,%.1f)\n", ix, iy, f.nearestDepth, stored,
					f.centerX, f.centerY, f.radiusX, f.radiusY);
			}
			// The user's exact case: the patch nearest screen center. The
			// screen center lands on a patch boundary (and this +z view is
			// mirrored: world -x appears right of center), so no fixed
			// (ix,iy) is exactly centered; take the closest footprint
			// (euclidean; pitch here is ~40x57px, nearest is ~29px away).
			const float dx = f.centerX - (float)W * 0.5f;
			const float dy = f.centerY - (float)H * 0.5f;
			const float dd = std::sqrt(dx * dx + dy * dy);
			if (dd < bestCenterDist) {
				bestCenterDist = dd;
				centerF = f;
			}
		}
	}
	Check(allKeep, "gfx.spawn.cloth-keeps");
	// Nearest patch is ~29px from center; pitch is ~40x57px, so 40.0
	// separates it cleanly from the next candidates (>= 42px).
	Check(!centerF.behindCamera && bestCenterDist < 40.0f, "gfx.spawn.center-is-center");
	{
		float stored = -1.0f;
		const bool occ = HzbOccluded(hzb.data(), W, H, mips, centerF, &stored);
		SMVK_DEBUG_LOG("[gfx.spawn] center patch: dNear=%.6f stored=%.6f occ=%d "
			"(pole margin %.2fx)\n", centerF.nearestDepth, stored, occ ? 1 : 0,
			stored > 0.0f ? centerF.nearestDepth / stored : 0.0f);
		Check(!occ, "gfx.spawn.center-keeps");
	}
	// Wall patch behind the cloth interior must cull (harness sees both ways).
	{
		ClusterFootprint f = ProjectClusterFootprint(glm::vec3(0.0f, 1.0f, 5.0f),
			0.3f, eye, viewProj, W, H);
		float stored = -1.0f;
		const bool occ = HzbOccluded(hzb.data(), W, H, mips, f, &stored);
		SMVK_DEBUG_LOG("[gfx.spawn] wall patch: dNear=%.6f stored=%.6f occ=%d\n",
			f.nearestDepth, stored, occ ? 1 : 0);
		Check(!f.behindCamera && occ, "gfx.spawn.wall-behind-cloth-culls");
	}
}

// ---------------------------------------------------------------------------
// GPU-mirror agreement: C++ port of gfxfilter.cs.hlsl ClusterOccluded.
//
// The shader derives its footprint/depth from conservative CLIP intervals
// (not the Euclidean nearest point the CPU ProjectClusterFootprint uses),
// so the two mirrors can disagree on borderline cases — but only in the
// safe direction (GPU keeps where the CPU culls). This test pins that:
//   - the canonical occluded case culls in BOTH mirrors (effectiveness:
//     occlusion can fire through the exact math the GPU executes);
//   - self-depth / sky / behind-camera / eye-inside keep in BOTH;
//   - fuzz: a GPU cull for an ON-SCREEN CPU-keep is forbidden (genuine
//     over-cull: visible geometry would vanish). The only allowed
//     divergence is a fully off-target CPU footprint, where the GPU
//     merely pre-empts the frustum stages (same outcome), with high
//     overall agreement.
// A regression here means the HLSL and the C++ mirror drifted apart, and
// the DumpIndirectAudit attribution (CPU mirror) no longer describes the
// GPU — the "occlusion not working but audit disagrees" class of bug.
// ---------------------------------------------------------------------------

namespace {
// Conservative n/w interval matching ConservativeRatioInterval in HLSL.
std::pair<float, float> GpuRatioInterval(float nCenter, float nRadius, float wMin, float wMax) {
	float r0 = (nCenter - nRadius) / wMin;
	float r1 = (nCenter - nRadius) / wMax;
	float r2 = (nCenter + nRadius) / wMin;
	float r3 = (nCenter + nRadius) / wMax;
	return { std::min(std::min(r0, r1), std::min(r2, r3)),
		std::max(std::max(r0, r1), std::max(r2, r3)) };
}
} // namespace

// Identity model (world-space sphere in, like the audit path with the model
// already applied). Mirrors ClusterOccluded in gfxfilter.cs.hlsl step by step.
bool GpuClusterOccluded(const glm::vec3& wc, float wr, const glm::vec3& eye,
	const glm::mat4& viewProj, uint32_t W, uint32_t H,
	const float* hzb, uint32_t mips) {
	using namespace smvk::gfx;
	if (hzb == nullptr || mips == 0 || W == 0 || H == 0)
		return false;
	const float width = (float)W, height = (float)H;
	const glm::vec3 toEye = eye - wc;
	const float dist = glm::length(toEye);
	if (!(dist > wr))
		return false; // eye inside: never cull
	const glm::vec4 clipC = viewProj * glm::vec4(wc, 1.0f);
	// HLSL viewProj._1C is row 0 (1-based row/col, layout-independent);
	// glm m[col][row], so row r is (m[0][r], m[1][r], m[2][r]).
	const float xRadius = wr * glm::length(glm::vec3(viewProj[0][0], viewProj[1][0], viewProj[2][0]));
	const float yRadius = wr * glm::length(glm::vec3(viewProj[0][1], viewProj[1][1], viewProj[2][1]));
	const float zRadius = wr * glm::length(glm::vec3(viewProj[0][2], viewProj[1][2], viewProj[2][2]));
	const float wRadius = wr * glm::length(glm::vec3(viewProj[0][3], viewProj[1][3], viewProj[2][3]));
	const float wMin = clipC.w - wRadius;
	const float wMax = clipC.w + wRadius;
	if (!(wMin > 1e-6f) || !(wMax >= wMin))
		return false;
	const auto ndcX = GpuRatioInterval(clipC.x, xRadius, wMin, wMax);
	const auto ndcY = GpuRatioInterval(clipC.y, yRadius, wMin, wMax);
	const auto ndcZ = GpuRatioInterval(clipC.z, zRadius, wMin, wMax);
	const float dNear = ndcZ.second;
	if (!(dNear >= 0.0f) || dNear > 1.0f)
		return false;
	const float x0 = (ndcX.first * 0.5f + 0.5f) * width;
	const float x1 = (ndcX.second * 0.5f + 0.5f) * width;
	const float y0 = (ndcY.first * 0.5f + 0.5f) * height;
	const float y1 = (ndcY.second * 0.5f + 0.5f) * height;
	if (!(x1 >= x0) || !(y1 >= y0))
		return false;
	if (x1 < 0.0f || x0 > width || y1 < 0.0f || y0 > height)
		return false; // off-target: frustum stages own it
	const float sx0 = std::min(std::max(x0, 0.0f), width);
	const float sx1 = std::min(std::max(x1, 0.0f), width);
	const float sy0 = std::min(std::max(y0, 0.0f), height);
	const float sy1 = std::min(std::max(y1, 0.0f), height);
	const float rectW = std::max(sx1 - sx0, 0.0f);
	const float rectH = std::max(sy1 - sy0, 0.0f);
	if (!(rectW > 0.0f) || !(rectH > 0.0f))
		return false;
	uint32_t level = 0;
	float texel = 1.0f;
	const float rectMax = std::max(rectW, rectH);
	while (texel < rectMax && level + 1 < mips) {
		texel *= 2.0f;
		++level;
	}
	uint32_t lw = W, lh = H, off = 0;
	for (uint32_t k = 0; k < level; ++k) {
		off += lw * lh;
		lw = std::max((lw + 1u) >> 1, 1u);
		lh = std::max((lh + 1u) >> 1, 1u);
	}
	if (lw == 0 || lh == 0)
		return false;
	const uint32_t tx0 = std::min((uint32_t)std::max(std::floor(sx0 / texel), 0.0f), lw - 1);
	const uint32_t tx1 = std::min((uint32_t)std::max(std::floor(sx1 / texel), 0.0f), lw - 1);
	const uint32_t ty0 = std::min((uint32_t)std::max(std::floor(sy0 / texel), 0.0f), lh - 1);
	const uint32_t ty1 = std::min((uint32_t)std::max(std::floor(sy1 / texel), 0.0f), lh - 1);
	if (tx0 > tx1 || ty0 > ty1)
		return false;
	const float testDepth = dNear + std::max(1e-7f, std::abs(dNear) * 1e-5f);
	for (uint32_t ty = ty0; ty <= ty1; ++ty) {
		for (uint32_t tx = tx0; tx <= tx1; ++tx) {
			if (testDepth >= hzb[off + ty * lw + tx])
				return false;
		}
	}
	return true;
}

void TestGpuMirrorAgreement() {
	using namespace smvk::gfx;
	const glm::vec3 eye(0, 0, 4);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), 1.0f, 0.1f);
	proj[1][1] *= -1.0f; // engine Y-flip, mirrors UpdateFrameUniforms
	const glm::mat4 viewProj = proj * view;
	constexpr uint32_t W = 256, H = 256;
	const uint32_t mips = HzbMipCount(W, H);

	auto cpuVerdict = [&](const glm::vec3& c, float r, const float* hzb) {
		return HzbOccluded(hzb, W, H, mips,
			ProjectClusterFootprint(c, r, eye, viewProj, W, H));
	};

	// Uniform-near pyramid (0.9 everywhere): frontal unit sphere sits behind
	// it (dNear ~0.03) so BOTH mirrors must cull — occlusion can fire.
	std::vector<float> nearHzb(HzbTotalFloats(W, H), 0.9f);
	const glm::vec3 front(0, 0, 0);
	Check(cpuVerdict(front, 1.0f, nearHzb.data()), "gfx.gpumirror.cpu-culls-behind");
	Check(GpuClusterOccluded(front, 1.0f, eye, viewProj, W, H, nearHzb.data(), mips),
		"gfx.gpumirror.gpu-culls-behind");

	// All-far pyramid (sky): nothing is hidden in either mirror.
	std::vector<float> farHzb(HzbTotalFloats(W, H), 0.0f);
	Check(!cpuVerdict(front, 1.0f, farHzb.data()), "gfx.gpumirror.cpu-sky-keeps");
	Check(!GpuClusterOccluded(front, 1.0f, eye, viewProj, W, H, farHzb.data(), mips),
		"gfx.gpumirror.gpu-sky-keeps");

	// Degenerate inputs keep in both mirrors.
	Check(!GpuClusterOccluded(glm::vec3(0, 0, 3.5f), 1.0f, eye, viewProj, W, H,
		nearHzb.data(), mips), "gfx.gpumirror.gpu-inside-keeps");
	Check(!GpuClusterOccluded(glm::vec3(0, 0, 8.0f), 1.0f, eye, viewProj, W, H,
		nearHzb.data(), mips), "gfx.gpumirror.gpu-behind-keeps");
	Check(!GpuClusterOccluded(front, 1.0f, eye, viewProj, W, H, nullptr, mips),
		"gfx.gpumirror.gpu-null-keeps");

	// Fuzz over a sloped pyramid: GPU must never cull what the CPU keeps
	// (no over-cull divergence), with high overall agreement. Deterministic
	// LCG so failures reproduce exactly.
	std::vector<float> slopeHzb(HzbTotalFloats(W, H), 0.0f);
	for (uint32_t m = 0; m < mips; ++m) {
		uint32_t mw = 0, mh = 0;
		HzbMipDims(W, H, m, mw, mh);
		const uint32_t off = HzbMipOffsetFloats(W, H, m);
		for (uint32_t y = 0; y < mh; ++y)
			for (uint32_t x = 0; x < mw; ++x)
				slopeHzb[off + y * mw + x] =
					0.05f + 0.9f * (float(x + y) / float(mw + mh));
	}
	uint32_t rng = 0xC0FFEEu;
	auto rnd = [&]() {
		rng = rng * 1664525u + 1013904223u;
		return float(rng >> 8) / float(1u << 24);
	};
	uint32_t overCull = 0, agree = 0;
	constexpr uint32_t kSamples = 300;
	for (uint32_t i = 0; i < kSamples; ++i) {
		const glm::vec3 c(-6.0f + rnd() * 12.0f, -4.0f + rnd() * 8.0f,
			-12.0f + rnd() * 11.0f);
		const float r = 0.1f + rnd() * 0.7f;
		const ClusterFootprint f = ProjectClusterFootprint(c, r, eye, viewProj, W, H);
		const bool cpu = HzbOccluded(slopeHzb.data(), W, H, mips, f);
		const bool gpu = GpuClusterOccluded(c, r, eye, viewProj, W, H,
			slopeHzb.data(), mips);
		if (gpu && !cpu) {
			// Allowed divergence only: the CPU footprint is fully
			// off-target, i.e. the GPU merely pre-empts the frustum
			// stages (which own off-screen triangles and would drop
			// them anyway). A GPU cull for an ON-SCREEN CPU-keep is a
			// genuine over-cull (visible geometry vanishes).
			const bool cpuOffscreen = f.behindCamera
				|| f.centerX + f.radiusX < 0.0f || f.centerX - f.radiusX > (float)W
				|| f.centerY + f.radiusY < 0.0f || f.centerY - f.radiusY > (float)H;
			if (!cpuOffscreen) {
				++overCull;
				float stored = -1.0f;
				HzbOccluded(slopeHzb.data(), W, H, mips, f, &stored);
				SMVK_DEBUG_LOG("[gfx.gpumirror] OVERCULL c=(%.3f,%.3f,%.3f) r=%.3f "
					"cpu c=(%.1f,%.1f) r=(%.1f,%.1f) dNear=%.6f stored=%.6f\n",
					c.x, c.y, c.z, r, f.centerX, f.centerY, f.radiusX, f.radiusY,
					f.nearestDepth, stored);
			}
		}
		if (gpu == cpu)
			++agree;
	}
	SMVK_DEBUG_LOG("[gfx.gpumirror] fuzz: %u/%u agree, %u over-cull\n",
		agree, kSamples, overCull);
	Check(overCull == 0, "gfx.gpumirror.no-overcull");
	Check(agree >= kSamples * 9 / 10, "gfx.gpumirror.agreement");
}

// ---------------------------------------------------------------------------
// T2x jitter vs frustum culling stability.
//
// The app jitters the main projection by +/-0.25 px every frame (SMAA T2x,
// see SceneRenderer::UpdateFrameUniforms). Exact plane tests drop/re-add
// boundary clusters on alternating frames (visible popping); the main view
// therefore carries a 1 px conservative guard band (kMainViewCullGuardPx,
// RenderViewSet::AllocateMainView guard arg). This test sweeps a small
// sphere across the right frustum edge in fine steps and checks, for both
// jitter phases, that:
//   - exact (guard 0) views DISAGREE on at least one step (the test setup
//     reproduces the flicker, so a zero guarded count is meaningful), and
//   - guarded views NEVER disagree (the fix kills the flicker), only ever
//     keep more (conservative), and still cull far-outside geometry.
// Deterministic: fixed grid, no RNG.
// ---------------------------------------------------------------------------

static glm::mat4 JitteredProjForTest(const glm::mat4& proj, float jxPx, float jyPx,
	float w, float h) {
	glm::mat4 j = proj;
	const float jx = jxPx * 2.0f / w;
	const float jy = jyPx * 2.0f / h;
	for (int c = 0; c < 4; ++c) {
		j[c][0] += jx * j[c][3];
		j[c][1] += jy * j[c][3];
	}
	return j;
}

void TestJitterCullStability() {
	constexpr float W = 800.0f, H = 600.0f;
	constexpr float kGuardPx = 1.0f;
	constexpr float kPxNdcX = 2.0f / W;

	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), W / H, 0.1f);
	proj[1][1] *= -1.0f; // app Vulkan Y-flip (cf. UpdateFrameUniforms)
	const glm::mat4 view = glm::mat4(1.0f); // camera at origin, looking -Z

	// App T2x phases, measured in render (internal) pixels.
	const glm::mat4 vpA = JitteredProjForTest(proj, +0.25f, +0.25f, W, H) * view;
	const glm::mat4 vpB = JitteredProjForTest(proj, -0.25f, -0.25f, W, H) * view;

	auto keepsSphere = [](const glm::mat4& vp, const glm::vec3& c, float r, float guardPx) {
		smvk::RenderViewSet views;
		views.NewFrame();
		const uint32_t rv = views.AllocateMainView(vp, { W, H }, guardPx);
		return smvk::RenderViewIntersectsSphere(views.Get(rv), c, r);
	};

	// Small sphere (~1 px at depth 5) swept across the right edge.
	// A 1 px guard cannot remove the keep/cull boundary itself (it moves it
	// outward); what it guarantees is VISIBLE stability: anything the exact
	// test keeps in either phase (hence potentially rasterized) is kept by
	// the guarded test in both phases. Guarded-only disagreements live
	// fully outside both nominal frusta and are rasterizer-clipped in every
	// phase, i.e. identical (invisible) images.
	const float fx = std::abs(proj[0][0]);
	const float d = 5.0f;
	const float r = 0.01f;
	uint32_t exactDisagree = 0, visibleFlicker = 0, guardOverCull = 0;
	constexpr int kSteps = 6001;
	for (int i = 0; i < kSteps; ++i) {
		const float xNdc = 1.0f + (float(i) / float(kSteps - 1) * 6.0f - 3.0f) * kPxNdcX;
		const glm::vec3 c(xNdc * d / fx, 0.0f, -d);
		const bool eA = keepsSphere(vpA, c, r, 0.0f);
		const bool eB = keepsSphere(vpB, c, r, 0.0f);
		const bool gA = keepsSphere(vpA, c, r, kGuardPx);
		const bool gB = keepsSphere(vpB, c, r, kGuardPx);
		if (eA != eB)
			++exactDisagree;
		if ((eA || eB) && !(gA && gB))
			++visibleFlicker; // visible in a phase, dropped in another
		if ((!gA && eA) || (!gB && eB))
			++guardOverCull; // guard must only ever keep more
	}
#ifndef NDEBUG
	std::cout << "[gfx.jitter] exact-disagree=" << exactDisagree
		<< " visible-flicker=" << visibleFlicker
		<< " guard-overcull=" << guardOverCull << "\n";
#endif
	Check(exactDisagree > 0, "gfx.jitter.setup-reproduces-flicker");
	Check(visibleFlicker == 0, "gfx.jitter.guard-kills-visible-flicker");
	Check(guardOverCull == 0, "gfx.jitter.guard-conservative");

	// Far-outside geometry still culls with the guard on (both phases).
	const glm::vec3 far(1.5f * d / fx, 0.0f, -d);
	Check(!keepsSphere(vpA, far, r, kGuardPx), "gfx.jitter.guard-still-culls-a");
	Check(!keepsSphere(vpB, far, r, kGuardPx), "gfx.jitter.guard-still-culls-b");
	// Deep-inside geometry still keeps without the guard (sanity).
	const glm::vec3 inside(0.0f, 0.0f, -d);
	Check(keepsSphere(vpA, inside, r, 0.0f), "gfx.jitter.exact-keeps-inside-a");
	Check(keepsSphere(vpB, inside, r, 0.0f), "gfx.jitter.exact-keeps-inside-b");
}

} // namespace

int main() {
	TestWindingSign();
	TestRegistration();
	TestBatching();
	TestLobeGrouping();
	TestPreCull();
	TestTwoSided();
	TestTemporalSnapshot();
	TestHizLayout();
	TestSeedCommands();
	TestSeedIntersection();
	TestHzbOcclusion();
	TestHzbSpawnView();
	TestGpuMirrorAgreement();
	TestRemainderCommands();
	TestVisKeyMerge();
	TestSeedDuplicateBatches();
	TestJitterCullStability();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
