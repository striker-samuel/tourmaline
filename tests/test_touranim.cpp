// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// touranim_tests: the .tourAnim ASCII loader + the spline sampler + the
// Cluster pose driver (SampleAnimation: track writes -> local compose ->
// global walk -> palette), headless (no GPU, nothing constructs a
// ResourceManager). Run via CTest.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#include <Engine/App/cluster.h>
#include <Engine/Asset/touranim.h>
#include <Engine/Scene/scene.h>

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

bool Near(float a, float b, float eps = 1e-3f) {
	return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------------------
// Text loader: the grammar in touranim.h's header comment (clips, tracks,
// keys; rotate = unit quat xyzw, translate/scale = 3 floats).
// ---------------------------------------------------------------------------
void TestLoadText() {
	const std::string text =
		"// .tourAnim 1 - unit test\n"
		"source -p \"test.gltf\";\n"
		"clip -name \"spin\" -duration 2.0 -fps 30;\n"
		"track -joint 1 -name \"arm\" -attr rotate -interp linear;\n"
		"  0.0000  0.0000 0.0000 0.0000 1.0000;\n"
		"  1.0000  0.0000 0.0000 0.707107 0.707107;\n"
		"  2.0000  0.0000 0.0000 1.000000 0.000000;\n"
		"track -joint 1 -attr translate -interp linear;\n"
		"  0.0000  1.000 0.000 0.000;\n"
		"  2.0000  3.000 0.000 0.000;\n"
		"track -joint 2 -attr scale -interp step;\n"
		"  0.0000  1.000 1.000 1.000;\n"
		"  2.0000  2.000 2.000 2.000;\n";
	smvk::touranim::AnimDoc doc;
	std::string err;
	std::vector<std::string> warns;
	Check(smvk::touranim::LoadTourAnimText(text, doc, err, &warns),
		"anim.load.ok");
	Check(err.empty(), "anim.load.noerr");
	Check(doc.source == "test.gltf", "anim.load.source");
	Check(doc.clips.size() == 1, "anim.load.clipcount");
	if (doc.clips.size() == 1) {
		const smvk::touranim::Clip& c = doc.clips.front();
		Check(c.name == "spin", "anim.load.clipname");
		Check(Near(float(c.duration), 2.0f), "anim.load.duration");
		Check(c.tracks.size() == 3, "anim.load.trackcount");
		if (c.tracks.size() == 3) {
			Check(c.tracks[0].keys.size() == 3, "anim.load.rotkeys");
			Check(c.tracks[0].channel == smvk::touranim::Channel::Rotate,
				"anim.load.rotchan");
			Check(c.tracks[0].spline == smvk::touranim::Spline::Linear,
				"anim.load.rotinterp");
			// The quat xyzw contract: the first key (0,0,0,1) parses as
			// v.x=0, v.y=0, v.z=0, v.w=1 (identity).
			Check(Near(c.tracks[0].keys[0].v.x, 0.0f)
				&& Near(c.tracks[0].keys[0].v.y, 0.0f)
				&& Near(c.tracks[0].keys[0].v.z, 0.0f)
				&& Near(c.tracks[0].keys[0].v.w, 1.0f),
				"anim.load.quatxyzw");
			Check(c.tracks[1].channel == smvk::touranim::Channel::Translate,
				"anim.load.trchan");
			Check(c.tracks[2].spline == smvk::touranim::Spline::Step,
				"anim.load.stepinterp");
		}
	}
	Check(warns.empty(), "anim.load.nowarns");
	for (const std::string& w : warns)
		std::cerr << "[touranim-tests] warn: " << w << "\n";
}

// ---------------------------------------------------------------------------
// Sampler: clamps, linear/nlerp, step.
// ---------------------------------------------------------------------------
void TestSampler() {
	using namespace smvk::touranim;
	Track rot{};
	rot.channel = Channel::Rotate;
	rot.spline = Spline::Linear;
	rot.keys = { { 0.0, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) },
		{ 1.0, glm::vec4(0.0f, 0.0f, 0.707107f, 0.707107f) } };
	// Clamp before/after.
	const glm::vec4 pre = SampleTrack(rot, -1.0);
	Check(Near(pre.w, 1.0f) && Near(pre.z, 0.0f), "anim.sample.preclamp");
	const glm::vec4 post = SampleTrack(rot, 2.0);
	Check(Near(post.z, 0.707107f) && Near(post.w, 0.707107f),
		"anim.sample.postclamp");
	// Linear midpoint: nlerp (0,0,0,1) -> (0,0,.7071,.7071) at f=.5 =
	// (0,0,.3827,.9239) = 45 degrees about Z (renormalized mix).
	const glm::vec4 mid = SampleTrack(rot, 0.5);
	Check(Near(mid.z, 0.38268f, 2e-3f) && Near(mid.w, 0.92388f, 2e-3f),
		"anim.sample.nlerp");
	// Unit length preserved.
	Check(Near(glm::length(mid), 1.0f, 1e-5f), "anim.sample.unit");
	// Shortest-path flip: the 180-degree opposite hemisphere key.
	Track flip{};
	flip.channel = Channel::Rotate;
	flip.spline = Spline::Linear;
	flip.keys = { { 0.0, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) },
		{ 1.0, glm::vec4(0.0f, 0.0f, -0.707107f, -0.707107f) } };
	const glm::vec4 fm = SampleTrack(flip, 0.5);
	// Flipped to the same hemisphere as key0: the long way never shows.
	Check(Near(fm.z, 0.38268f, 2e-3f) && Near(fm.w, 0.92388f, 2e-3f),
		"anim.sample.shortestpath");
	// Translate linear + step hold.
	Track tr{};
	tr.channel = Channel::Translate;
	tr.spline = Spline::Linear;
	tr.keys = { { 0.0, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) },
		{ 2.0, glm::vec4(3.0f, 0.0f, 0.0f, 1.0f) } };
	const glm::vec4 tv = SampleTrack(tr, 1.0);
	Check(Near(tv.x, 2.0f), "anim.sample.linear");
	Track st{};
	st.channel = Channel::Scale;
	st.spline = Spline::Step;
	st.keys = { { 0.0, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ 2.0, glm::vec4(2.0f, 2.0f, 2.0f, 1.0f) } };
	const glm::vec4 sv = SampleTrack(st, 1.999);
	Check(Near(sv.x, 1.0f), "anim.sample.step");
	// Catmull-Rom: symmetric mid of a symmetric arc hits the middle key.
	Track cr{};
	cr.channel = Channel::Translate;
	cr.spline = Spline::CatmullRom;
	cr.keys = { { 0.0, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) },
		{ 1.0, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) },
		{ 2.0, glm::vec4(2.0f, 0.0f, 0.0f, 1.0f) },
		{ 3.0, glm::vec4(3.0f, 0.0f, 0.0f, 1.0f) } };
	const glm::vec4 cv = SampleTrack(cr, 1.0);
	Check(Near(cv.x, 1.0f), "anim.sample.catmullrom");
	// Empty track: the documented zero default.
	Track empty{};
	const glm::vec4 ev = SampleTrack(empty, 1.0);
	Check(Near(ev.x, 0.0f) && Near(ev.w, 0.0f), "anim.sample.empty");
}

// ---------------------------------------------------------------------------
// Cluster pose driver: SampleAnimation advances the clock, writes the
// joint locals, and the palette moves; no clip = bind-pose identity.
// The regression this pins: the driver once froze at the first-key pose
// (observed in the knight demo - identical output every frame).
// ---------------------------------------------------------------------------
void TestPoseDriver() {
	std::cerr << "[touranim-tests] pose: build cluster\n";
	// A 2-node chain: node0 root (identity) -> node1 arm, bind local =
	// translate(1,0,0). Skeleton: joints {0,1}, identity inverse binds
	// (palette[i] = global[i] at the bind pose).
	auto c = std::make_unique<smvk::Cluster>(nullptr);
	c->SetOfflineMode(true);
	std::cerr << "[touranim-tests] pose: cluster made\n";
	// addNode (never hand-built vectors): it keeps EVERY parallel array
	// sized in lockstep (localTransform + globalTransform + the static/
	// lightmap/visibility caches) - a hand-built scene that forgets one
	// (globalTransform) crashes the first global walk with an OOB write.
	addNode(c->scene, -1, 0);
	addNode(c->scene, 0, 1);
	c->scene.localTransform[1] = glm::translate(glm::mat4(1.0f),
		glm::vec3(1.0f, 0.0f, 0.0f));
	std::cerr << "[touranim-tests] pose: nodes made\n";
	markAsChanged(c->scene, 0);
	recalculateGlobalTransforms(c->scene);
	std::cerr << "[touranim-tests] pose: globals ok\n";
	c->skeleton.joints = { 0u, 1u };
	c->skeleton.inverseBind.assign(2, glm::mat4(1.0f));
	c->skeleton.root = 0;
	c->hasSkeleton = true;
	std::cerr << "[touranim-tests] pose: scene built\n";
	// Bind pose without a clip: false + identity palette.
	std::vector<glm::mat4> pal;
	Check(!c->SampleAnimation(1.0f / 60.0f, pal), "anim.pose.noclip");
	smvk::touranim::BuildSkinPalette(c->scene, c->skeleton, pal);
	Check(pal.size() == 2 && Near(pal[1][3].x, 1.0f), "anim.pose.bindpose");
	std::cerr << "[touranim-tests] pose: bind pose ok\n";
	// A Z-rotation clip on the arm: identity -> 90deg about Z over 1s.
	smvk::touranim::Clip clip;
	clip.name = "spin";
	clip.duration = 1.0;
	smvk::touranim::Track t;
	t.joint = 1;
	t.channel = smvk::touranim::Channel::Rotate;
	t.spline = smvk::touranim::Spline::Linear;
	t.keys = { { 0.0, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) },
		{ 1.0, glm::vec4(0.0f, 0.0f, 0.707107f, 0.707107f) } };
	clip.tracks.push_back(t);
	c->SetAnimClip(std::move(clip));
	std::cerr << "[touranim-tests] pose: clip attached\n";
	// Advance to t=0.5 (three 1/60 steps then one 0.45 hop): 45deg about Z.
	c->SampleAnimation(1.0f / 60.0f, pal);
	c->SampleAnimation(1.0f / 60.0f, pal);
	c->SampleAnimation(1.0f / 60.0f, pal);
	c->SampleAnimation(0.45f, pal);
	Check(pal.size() == 2, "anim.pose.sizerot");
	// palette[1] = T(1,0,0) * Rz(45): the local x-axis maps to
	// (cos45, sin45, 0) in the global frame.
	Check(Near(pal[1][0].x, 0.70711f, 2e-3f)
		&& Near(pal[1][0].y, 0.70711f, 2e-3f)
		&& Near(pal[1][0].z, 0.0f), "anim.pose.rot45");
	Check(Near(pal[1][3].x, 1.0f), "anim.pose.rot45origin");
	// Advance to the end: 90deg.
	c->SampleAnimation(0.5f, pal);
	Check(Near(pal[1][0].y, 1.0f, 2e-3f), "anim.pose.rot90");
	// Duration wrap: past the end the clock folds back into the clip.
	c->SampleAnimation(0.5f, pal);
	Check(Near(pal[1][0].y, 0.70711f, 2e-3f)
		&& Near(pal[1][0].x, 0.70711f, 2e-3f), "anim.pose.wrap");
	// THE FREEZE REGRESSION: successive samples differ.
	std::vector<glm::mat4> a, b;
	c->SampleAnimation(0.25f, a);
	c->SampleAnimation(0.25f, b);
	bool differs = false;
	for (int col = 0; col < 4 && !differs; ++col)
		for (int row = 0; row < 4; ++row)
			if (!Near(a[1][col][row], b[1][col][row], 1e-4f)) {
				differs = true;
				break;
			}
	Check(differs, "anim.pose.advances");
	std::cerr << "[touranim-tests] pose: checks done\n";
}

} // namespace

int main() {
#ifdef _MSC_VER
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
	std::cerr << "[touranim-tests] begin\n";
	TestLoadText();
	std::cerr << "[touranim-tests] load ok\n";
	TestSampler();
	std::cerr << "[touranim-tests] sampler ok\n";
	TestPoseDriver();
	std::cerr << "[touranim-tests] pose ok\n";
	std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
	std::cout << (g_failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
	return g_failures == 0 ? 0 : 1;
}
