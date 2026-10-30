// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// scene_table_tests (Epoch 3 of the xForNode migration —
// docs/scene_tables_migration.md): the typed-table maintenance
// contract — insertion, lookup, replacement, duplicate rejection
// (replace semantics), removal with swap-and-pop lookup repair,
// dead-row compaction on rebuild, per-kind coexistence, and the
// derived rowsForNode index staying consistent with the tables.
//
// The tables are not yet authoritative (the maps are, until Epochs
// 4-5) — nothing here touches the maps.
//
// Hand-rolled runner (the house convention): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cstdint>
#include <cstdio>

#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond)
		std::printf("PASS %s\n", name);
	else {
		++g_failures;
		std::printf("FAIL %s\n", name);
	}
}

// A fresh scene with `count` root-level nodes (addNode grows
// hierarchy/transforms + the seven flag tables; rowsForNode is
// DELIBERATELY not grown by addNode — the mutators grow it lazily).
Scene MakeScene(uint32_t count) {
	Scene scene;
	for (uint32_t i = 0; i < count; ++i)
		addNode(scene, -1, 0);
	return scene;
}

} // namespace

int main() {
	{
		// ---- insertion + lookup ----
		Scene s = MakeScene(4);
		Check(SetMesh(s, 1, 10), "SetMesh valid node returns true");
		Check(SetMaterial(s, 1, 20), "SetMaterial valid node returns true");
		const MeshRow* mr = FindMesh(s, 1);
		Check(mr != nullptr, "FindMesh finds the row");
		Check(mr && mr->node == 1 && mr->mesh == 10,
			"mesh row payload round-trips");
		const MaterialRow* matr = FindMaterial(s, 1);
		Check(matr && matr->material == 20,
			"material row payload round-trips");
		// The two rows are INDEPENDENT (tour assignMaterial pins
		// material-without-mesh; gltf instancing pins mesh-without-
		// material — one owner per fact).
		Scene s2 = MakeScene(2);
		SetMaterial(s2, 0, 7);
		Check(FindMaterial(s2, 0) && !FindMesh(s2, 0),
			"material row exists without a mesh row");
		SetMesh(s2, 1, 8);
		Check(FindMesh(s2, 1) && !FindMaterial(s2, 1),
			"mesh row exists without a material row");
		Check(RemoveMesh(s2, 1) && FindMaterial(s2, 1) == nullptr
			&& FindMesh(s2, 1) == nullptr,
			"RemoveMesh leaves other kinds alone");
		Check(FindMesh(s, 0) == nullptr, "node without a row finds null");
		Check(FindMesh(s, 999) == nullptr, "out-of-range node finds null");

		// ---- duplicate rejection = replacement ----
		Check(SetMesh(s, 1, 11), "SetMesh on an associated node succeeds");
		Check(s.tables.meshes.size() == 1, "re-Set does not add a second row");
		auto mr2 = FindMesh(s, 1);
		Check(mr2 && mr2->mesh == 11, "re-Set replaces the payload in place");
		Check(SetMaterial(s, 1, 21) && s.tables.materials.size() == 1
			&& FindMaterial(s, 1)->material == 21,
			"re-SetMaterial replaces in place");

		// ---- invalid-node Set is rejected ----
		Check(!SetMesh(s, 999, 1), "SetMesh out-of-range node returns false");
		Check(!SetLight(s, 4, 7), "SetLight node == count returns false");

		// ---- removal + swap-and-pop lookup repair ----
		SetLight(s, 0, 100);
		SetLight(s, 2, 102);
		SetLight(s, 3, 103);
		Check(s.tables.lights.size() == 3, "three light rows live");
		Check(RemoveLight(s, 0), "RemoveLight on an associated node returns true");
		Check(s.tables.lights.size() == 2, "swap-and-pop keeps the table dense");
		// Row 0 was freed; the LAST row (node 3) moved into it — its
		// lookup must still resolve with the ORIGINAL payload.
		const LightRow* lr = FindLight(s, 3);
		Check(lr != nullptr, "the moved row stays findable");
		Check(lr && lr->light == 103, "the moved row keeps its payload");
		Check(FindLight(s, 0) == nullptr, "the removed node finds null");
		Check(!RemoveLight(s, 0), "re-Remove returns false (no row)");
		Check(FindLight(s, 2) && FindLight(s, 2)->light == 102,
			"untouched rows are unaffected by the swap");

		// ---- per-kind coexistence on one node ----
		SetMesh(s, 2, 30);
		SetMaterial(s, 2, 32);
		SetDecal(s, 2, 40);
		SetCamera(s, 2, 50);
		Check(FindMesh(s, 2) && FindMaterial(s, 2) && FindDecal(s, 2)
			&& FindCamera(s, 2) && FindLight(s, 2),
			"all five kinds coexist on one node");
		Check(FindDecal(s, 2)->decal == 40 && FindCamera(s, 2)->camera == 50,
			"coexisting rows keep their own payloads");

		// ---- RemoveNodeRows drops every kind at once ----
		Check(RemoveNodeRows(s, 2), "RemoveNodeRows reports removal");
		Check(!FindMesh(s, 2) && !FindMaterial(s, 2) && !FindDecal(s, 2)
			&& !FindCamera(s, 2) && !FindLight(s, 2),
			"RemoveNodeRows cleared all kinds");
		Check(!RemoveNodeRows(s, 2), "RemoveNodeRows on a bare node is false");

		// ---- RebuildNodeRows reconstructs the index from the tables ----
		// (direct table manipulation is legal — the rebuild is the
		// recovery path; here it must reproduce the exact same index.)
		const size_t meshesBefore = s.tables.meshes.size();
		RebuildNodeRows(s);
		Check(s.tables.meshes.size() == meshesBefore,
			"rebuild preserves in-range rows");
		Check(FindMesh(s, 1) && FindMesh(s, 1)->mesh == 11,
			"rebuild reproduces the mesh index");
	}

	{
		// ---- dead-row compaction on rebuild ----
		// Rows outliving the node count (a compacting deletion shrank
		// the scene) are unreachable and get compacted out by the
		// rebuild — "a table guarantees valid nodes".
		Scene s = MakeScene(4);
		SetMesh(s, 0, 1);
		SetMesh(s, 3, 3);
		SetDecal(s, 3, 9);
		// Simulate the shrink: two nodes live, rows for 2 and 3 are dead.
		s.hierarchy.resize(2);
		s.localTransform.resize(2);
		s.globalTransform.resize(2);
		RebuildNodeRows(s);
		Check(s.tables.meshes.size() == 1,
			"rebuild compacts out-of-range mesh rows");
		Check(s.tables.decals.empty(),
			"rebuild compacts out-of-range decal rows");
		Check(FindMesh(s, 0) && FindMesh(s, 0)->mesh == 1,
			"the surviving row stays indexed");
		Check(!FindMesh(s, 1) && s.rowsForNode.size() == 2,
			"the index matches the shrunken node count");
	}

	{
		// ---- NodeRows defaults + the derived-index contract ----
		Scene s = MakeScene(1);
		Check(s.rowsForNode.empty(),
			"a fresh scene carries no derived index (lazy growth)");
		RebuildNodeRows(s);
		Check(s.rowsForNode.size() == 1
			&& s.rowsForNode[0].mesh == InvalidRow
			&& s.rowsForNode[0].material == InvalidRow
			&& s.rowsForNode[0].light == InvalidRow
			&& s.rowsForNode[0].camera == InvalidRow
			&& s.rowsForNode[0].decal == InvalidRow
			&& s.rowsForNode[0].body == InvalidRow,
			"NodeRows defaults are all InvalidRow (body reserved for Epoch 10)");
	}

	if (g_failures > 0) {
		std::printf("scene_table_tests: %d/%d checks failed\n",
			g_failures, g_checks);
		return 1;
	}
	std::printf("scene_table_tests: all %d checks passed\n", g_checks);
	return 0;
}
