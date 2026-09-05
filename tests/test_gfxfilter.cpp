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
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Engine/Renderer/gfx_filter.h>
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
			std::cout << "[gfx.wind] inward tri " << (t / 3)
				<< " a=(" << a.x << "," << a.y << "," << a.z << ")"
				<< " b=(" << b.x << "," << b.y << "," << b.z << ")"
				<< " c=(" << c.x << "," << c.y << "," << c.z << ")"
				<< " dot=" << glm::dot(n, center) << "\n";
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
	std::cout << "[gfx.wind] tris=" << total << " counted=" << counted
		<< " agreeFlip=" << fracFlip << " agreeNoFlip=" << fracNoFlip
		<< " geoPixel=" << fracGeoPixel
		<< " detPixelNoFlip=" << fracDetNoFlip << " detPixelFlip=" << fracDetFlip << "\n";
	Check(counted > total * 7 / 10, "gfx.wind.meaningful-subset");
	Check(fracGeoPixel > 0.98, "gfx.wind.ground-truth-matches-rasterizer");
	// Exactly one of these must hold; whichever does selects the flag.
	const bool noFlipRight = fracDetNoFlip > 0.98 && fracDetFlip < 0.05;
	const bool flipRight = fracDetFlip > 0.98 && fracDetNoFlip < 0.05;
	std::cout << "[gfx.wind] noFlipRight=" << noFlipRight << " flipRight=" << flipRight << "\n";
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
		std::cout << "[gfx.cull] front: kept=" << out.batchCount
			<< " culled=" << out.stats.clustersCulled
			<< " processed=" << out.stats.clustersProcessed << "\n";
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
		std::cout << "[gfx.twosided] single=" << singleKept
			<< " double=" << out.batchCount << " total=" << clusterTotal << "\n";
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
	Check(HzbMipCount(1920, 1080) == 11, "gfx.hiz.hd-levels");

	uint32_t w = 0, h = 0;
	HzbMipDims(1920, 1080, 0, w, h);
	Check(w == 1920 && h == 1080, "gfx.hiz.dims-mip0");
	HzbMipDims(1920, 1080, 1, w, h);
	Check(w == 960 && h == 540, "gfx.hiz.dims-mip1");
	HzbMipDims(1920, 1080, 10, w, h);
	Check(w == 1 && h == 1, "gfx.hiz.dims-last");

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
		Check(total == HzbMipOffsetFloats(1920, 1080, 10) + lw * lh,
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

	// Invalidating clears the seed list (no stale commands survive).
	InvalidateHistory(s, 0);
	BuildSeedCommands(s, 0);
	Check(Temporal(s, 0).seedCommands.empty()
		&& Temporal(s, 0).seedRanges.empty(),
		"gfx.seed.invalidate-clears");
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
	Check(f.radiusX > 150.0f && f.radiusX < 200.0f
		&& f.radiusY > 110.0f && f.radiusY < 150.0f, "gfx.occ.front-radii");
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
}

} // namespace

int main() {
	TestWindingSign();
	TestRegistration();
	TestBatching();
	TestPreCull();
	TestTwoSided();
	TestTemporalSnapshot();
	TestHizLayout();
	TestSeedCommands();
	TestHzbOcclusion();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
