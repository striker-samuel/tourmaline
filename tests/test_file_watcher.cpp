// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// file_watcher_tests: CPU-only regression tests for the project-scoped
// native filesystem watcher (Engine/Utils/file_watcher.h): the ignore
// policy (generated artifacts invisible wherever they sit), the
// debounce/settle contract, add/remove/modify classification,
// atomic-save renames, and the reconcile recovery (missed events,
// restart). Timing tests use a short settle window; the watcher's
// thread carries the load, the test only polls.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <Engine/Utils/file_watcher.h>

namespace {

namespace fs = std::filesystem;

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

void WriteFile(const fs::path& p, const std::string& text) {
	std::error_code ec;
	fs::create_directories(p.parent_path(), ec);
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	f << text;
}

using Change = smvk::FileWatcher::Change;

bool HasEvent(const std::vector<Change>& changes, const fs::path& p,
	smvk::FileWatcher::Event ev) {
	const std::string want =
		p.lexically_normal().generic_string();
	for (const Change& c : changes)
		if (c.path == want && c.event == ev)
			return true;
	return false;
}

// Poll until quiet (settle window + margin), gathering everything.
std::vector<Change> Drain(smvk::FileWatcher& w, int settleMs,
	int maxWaitMs) {
	std::vector<Change> all;
	const auto t0 = std::chrono::steady_clock::now();
	for (;;) {
		std::vector<Change> batch = w.Poll(settleMs);
		all.insert(all.end(), batch.begin(), batch.end());
		if (std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - t0)
				.count() >= maxWaitMs)
			return all;
		std::this_thread::sleep_for(
			std::chrono::milliseconds(settleMs / 2 + 8));
	}
}

void WriteAndWait(smvk::FileWatcher& w, const fs::path& p,
	const std::string& text, int settleMs) {
	WriteFile(p, text);
	(void)Drain(w, settleMs, settleMs * 8);
}

void TestPolicy() {
	const smvk::FileWatcher::Policy pol =
		smvk::FileWatcher::Policy::ProjectDefault();
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_fw_policy";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp / "content" / "models" / "knight", ec);
	fs::create_directories(tmp / "content" / "models" / "knight"
		/ "shared", ec);
	fs::create_directories(tmp / ".tourmaline" / "stage", ec);
	fs::create_directories(tmp / "paks", ec);
	// Authoritative sources REPORT.
	Check(!pol.Ignored((tmp / "content" / "models" / "knight"
			/ "knight.gltf").generic_string()),
		"policy.watches-gltf-source");
	Check(!pol.Ignored((tmp / "content" / "models" / "knight"
			/ "knight.gltf.meta").generic_string()),
		"policy.watches-identity-sidecar");
	Check(!pol.Ignored((tmp / "content" / "scenes" / "scene.tour")
			.generic_string()),
		"policy.watches-tour");
	Check(!pol.Ignored((tmp / "game.tourrecipe").generic_string()),
		"policy.watches-recipe");
	Check(!pol.Ignored((tmp / "content" / "models" / "knight"
			/ "shared" / "stone.png").generic_string()),
		"policy.watches-texture-source");
	Check(!pol.Ignored((tmp / "content" / "shaders" / "sky.frag.hlsl")
			.generic_string()),
		"policy.watches-shader-source");
	Check(!pol.Ignored((tmp / "content" / "shaders"
			/ "material_evaluation.hlsli").generic_string()),
		"policy.watches-shader-include");
	// Generated artifacts are INVISIBLE wherever they sit (the
	// derive-beside contract: derived families sit NEXT to sources).
	Check(pol.Ignored((tmp / "content" / "models" / "knight"
			/ "knight.geometry").generic_string()),
		"policy.ignores-derived-geometry");
	Check(pol.Ignored((tmp / "content" / "models" / "knight"
			/ "knight.mat0_Grid.material").generic_string()),
		"policy.ignores-derived-material");
	Check(pol.Ignored((tmp / "content" / "models" / "knight"
			/ "knight.anim0_Walk.anim").generic_string()),
		"policy.ignores-derived-anim");
	Check(pol.Ignored((tmp / "content" / "shaders" / "sky.frag.spv")
			.generic_string()),
		"policy.ignores-compiled-shader");
	Check(pol.Ignored((tmp / "paks" / "edited.pak").generic_string()),
		"policy.ignores-pak");
	Check(pol.Ignored((tmp / "game.tapp").generic_string()),
		"policy.ignores-manifest");
	// Ignored DIRECTORY NAMES prune whole subtrees (staging, caches,
	// build trees) regardless of physical location.
	Check(pol.Ignored((tmp / ".tourmaline" / "stage" / "thing.bin")
			.generic_string()),
		"policy.ignores-staging-tree");
	Check(pol.Ignored((tmp / "content" / "out" / "x.spv")
			.generic_string()),
		"policy.ignores-build-tree-inside-content");
	// The generated header (no stable extension) rides the NAME rule.
	Check(pol.Ignored((tmp / "generated" / "GameProject" / "project.h")
			.generic_string()),
		"policy.ignores-generated-header");
	// Atomic-save temps.
	Check(pol.Ignored((tmp / "scene.tour~tmp").generic_string()),
		"policy.ignores-save-temp");
}

void TestWatchLifecycle() {
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_fw_life";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	const fs::path content = tmp / "content";
	fs::create_directories(content, ec);
	// The settle window the timing tests use (fast but realistic).
	constexpr int kSettle = 60;

	smvk::FileWatcher w;
	std::string err;
	Check(w.Start(smvk::FileWatcher::Policy::ProjectDefault(), err)
		&& w.Running(), "fw.starts");
	Check(w.AddDirectory(content.generic_string(), err), "fw.adds-dir");
	// The startup baseline: silent (the owner's own records decide
	// what the world implies).
	Check(w.Reconcile(err).empty(), "fw.baseline-silent");
	// Registration idempotence.
	Check(w.AddDirectory(content.generic_string(), err), "fw.adds-again");

	// ADD: a file that did not exist at the baseline reports Added
	// (create-then-write; the last classification wins).
	const fs::path tour = content / "scene.tour";
	WriteFile(tour, "v1");
	std::vector<Change> c1 = Drain(w, kSettle, kSettle * 12);
	Check(HasEvent(c1, tour, smvk::FileWatcher::Event::Added),
		"fw.first-write-added");
	Check(c1.size() == 1, "fw.single-settled-event");

	// MODIFY: a later write of the now-known file reports Modified.
	{
		std::ofstream f(tour, std::ios::binary | std::ios::trunc);
		f << "v2";
	}
	std::vector<Change> c1b = Drain(w, kSettle, kSettle * 12);
	Check(c1b.size() == 1
		&& HasEvent(c1b, tour, smvk::FileWatcher::Event::Modified),
		"fw.rewrite-modified");

	// DEBOUNCE: a burst of writes collapses to ONE event.
	for (int i = 0; i < 5; ++i) {
		std::ofstream f(tour, std::ios::binary | std::ios::trunc);
		f << "burst " << i;
		std::this_thread::sleep_for(
			std::chrono::milliseconds(kSettle / 3));
	}
	std::vector<Change> c2 = Drain(w, kSettle, kSettle * 12);
	Check(c2.size() == 1
		&& HasEvent(c2, tour, smvk::FileWatcher::Event::Modified),
		"fw.burst-debounces-to-one");

	// ADD: another brand-new file reports Added.
	const fs::path recipe = content / "game.tourrecipe";
	WriteFile(recipe, "recipe 1");
	std::vector<Change> c3 = Drain(w, kSettle, kSettle * 12);
	Check(HasEvent(c3, recipe, smvk::FileWatcher::Event::Added),
		"fw.new-file-added");

	// IGNORED outputs never report (the generated family lands BESIDE
	// the sources — no feedback loop).
	WriteAndWait(w, content / "scene.geometry", "blob", kSettle);
	WriteAndWait(w, content / "scene.pak", "pak", kSettle);
	Check(w.Poll(kSettle).empty(), "fw.ignored-outputs-silent");

	// ATOMIC-SAVE (write temp + rename over the target): the temp is
	// filtered, the target lands ONE Modified.
	{
		const fs::path tmpSave = content / "scene.tour.tmp";
		WriteFile(tmpSave, "v2-atomic");
		fs::rename(tmpSave, tour, ec);
	}
	std::vector<Change> c4 = Drain(w, kSettle, kSettle * 12);
	Check(c4.size() == 1
		&& HasEvent(c4, tour, smvk::FileWatcher::Event::Modified),
		"fw.atomic-save-one-modified");

	// REMOVE: deleting watched files reports Removed.
	fs::remove(recipe, ec);
	std::vector<Change> c5 = Drain(w, kSettle, kSettle * 12);
	Check(HasEvent(c5, recipe, smvk::FileWatcher::Event::Removed),
		"fw.delete-removed");

	// STOP/START: roots + state survive; events DURING the stop are
	// RECONCILE-visible (the missed-event recovery).
	w.Stop();
	Check(!w.Running(), "fw.stops");
	WriteFile(tour, "v3-while-stopped");
	const fs::path newSrc = content / "extra.tour";
	WriteFile(newSrc, "new while stopped");
	fs::remove(tour, ec);
	Check(w.Start(smvk::FileWatcher::Policy::ProjectDefault(), err)
		&& w.Running(), "fw.restarts");
	std::vector<Change> c6 = w.Reconcile(err);
	Check(HasEvent(c6, newSrc, smvk::FileWatcher::Event::Added),
		"fw.reconcile-sees-added");
	Check(HasEvent(c6, tour, smvk::FileWatcher::Event::Removed),
		"fw.reconcile-sees-removed");
	// A second reconcile over an unchanged world is quiet.
	Check(w.Reconcile(err).empty(), "fw.reconcile-quiet-when-current");

	// Single-FILE registration: a file root reports its own changes
	// but NOT its siblings.
	{
		const fs::path single = tmp / "single-root.txt";
		WriteFile(single, "x");
		smvk::FileWatcher w2;
		Check(w2.Start(smvk::FileWatcher::Policy{}, err),
			"fw.file-root-starts");
		// A policy with NO ignore rules (raw behavior).
		Check(w2.AddFile(single.generic_string(), err),
			"fw.file-root-adds");
		Check(w2.Reconcile(err).empty(), "fw.file-root-baseline");
		const fs::path sibling = tmp / "sibling.txt";
		WriteFile(sibling, "noise");
		std::ofstream sf(single, std::ios::binary | std::ios::trunc);
		sf << "changed";
		std::vector<Change> c7 = Drain(w2, kSettle, kSettle * 12);
		Check(HasEvent(c7, single, smvk::FileWatcher::Event::Modified)
			&& !HasEvent(c7, sibling,
				smvk::FileWatcher::Event::Modified),
			"fw.file-root-filters-siblings");
		w2.Stop();
	}
	w.Stop();
}

void TestBadRegistration() {
	smvk::FileWatcher w;
	std::string err;
	Check(!w.AddDirectory("Z:/definitely/not/here", err)
			&& !err.empty(), "fw.rejects-missing-dir");
	Check(!w.AddFile("Z:/definitely/not/here.txt", err)
			&& !err.empty(), "fw.rejects-missing-file");
	// Unstarted watcher: registration still works (arms at Start).
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_fw_pre";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	Check(w.AddDirectory(tmp.generic_string(), err), "fw.pre-arms");
	std::string serr;
	Check(w.Start(smvk::FileWatcher::Policy::ProjectDefault(), serr),
		"fw.pre-armed-starts");
	w.Stop();
}

} // namespace

int main() {
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	std::cout << std::unitbuf;
	TestPolicy();
	TestWatchLifecycle();
	TestBadRegistration();
	std::cout << "file_watcher_tests: "
		<< (g_checks - g_failures) << "/" << g_checks
		<< " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
