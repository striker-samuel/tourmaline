// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// scene_table_tests (Epoch 3 of the xForNode migration —
// docs/scene_tables_migration.md): the typed-table maintenance
// contract — insertion, lookup, replacement, duplicate rejection
// (replace semantics), removal with swap-and-pop lookup repair,
// dead-row compaction on rebuild, per-kind coexistence, and the
// derived rowsForNode index staying consistent with the tables.
// Since Epoch 6b: the STABLE-SLOT contract — retirement without
// renumbering, rows addressing live slots only, clean reuse, and the
// generational NodeHandle ABA guard.
//
// Hand-rolled runner (the house convention): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Engine/GI/cubemap_volume.h> // the GI parity reference
#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>
#include <Engine/Scene/spatial_volume.h>

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

bool Near(float a, float b, float eps = 1e-4f) {
	return std::fabs(a - b) <= eps;
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
		Scene s = MakeScene(4);
		tour::scene::ScenePhysicsBody dynamic{};
		dynamic.motion = 1;
		dynamic.shape = 2;
		dynamic.mass = 12.5f;
		dynamic.radius = 0.35f;
		Check(tour::scene::SetPhysicsBody(s, 1, dynamic), "physics: attach");
		Check(tour::scene::FindPhysicsBody(s, 1)
			&& tour::scene::FindPhysicsBody(s, 1)->body.mass == 12.5f,
			"physics: authored data stored");
		Check(tour::scene::SetPhysicsBody(s, 2, {}),
			"physics: second body attaches");
		Check(tour::scene::RemovePhysicsBody(s, 1),
			"physics: detach");
		Check(tour::scene::FindPhysicsBody(s, 2)
			&& tour::scene::FindPhysicsBody(s, 2)->node == 2,
			"physics: swap-and-pop fixes node lookup");
		Check(tour::scene::SetPhysicsBody(s, 2, dynamic)
			&& s.tables.physicsBodies.size() == 1,
			"physics: repeated attach replaces");
		RebuildNodeRows(s);
		Check(tour::scene::FindPhysicsBody(s, 2)
			&& Near(tour::scene::FindPhysicsBody(s, 2)->body.radius, 0.35f),
			"physics: index rebuild preserves body");
		Check(RemoveNodeRows(s, 2) && !tour::scene::FindPhysicsBody(s, 2),
			"physics: node deletion detaches body");
		Check(!tour::scene::SetPhysicsBody(s, 999, {}),
			"physics: cannot attach to invalid node");
	}
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
		SceneAudioSource audio;
		audio.volume = 0.8f;
		audio.minDistance = 2.0f;
		audio.sounds.push_back(AudioSoundRef{ "Bells",
			"content/audio/bells.wav", 0, 1.0f, true });
		SetAudioSource(s, 2, audio);
		tour::scene::SceneAudioRoom room;
		room.priority = 4;
		room.wet = 0.4f;
		room.rt60 = 2.5f;
		tour::scene::SetAudioRoom(s, 2, room);
		Check(FindMesh(s, 2) && FindMaterial(s, 2) && FindDecal(s, 2)
			&& FindCamera(s, 2) && FindLight(s, 2)
			&& FindAudioSource(s, 2) && tour::scene::FindAudioRoom(s, 2),
			"all kinds coexist on one node");
		Check(FindDecal(s, 2)->decal == 40 && FindCamera(s, 2)->camera == 50,
			"coexisting rows keep their own payloads");
		{
			const tour::scene::AudioRoomRow* rr = tour::scene::FindAudioRoom(s, 2);
			Check(rr && rr->room.priority == 4
				&& rr->room.wet == 0.4f && rr->room.rt60 == 2.5f,
				"audio room row payload round-trips");
			// A GI-volume row on the SAME node coexists (one placement,
			// two independent associations) — the room neither requires
			// nor disturbs it.
			Check(SetGiVolume(s, 2, SceneGiVolume{})
				&& FindGiVolume(s, 2) && tour::scene::FindAudioRoom(s, 2),
				"audio room + GI volume coexist on one node");
			RemoveGiVolume(s, 2);
			Check(tour::scene::FindAudioRoom(s, 2),
				"removing the GI row leaves the room intact");
			// Replacement in place (Set replaces).
			tour::scene::SceneAudioRoom room2;
			room2.priority = 7;
			Check(tour::scene::SetAudioRoom(s, 2, room2)
				&& s.tables.audioRooms.size() == 1
				&& tour::scene::FindAudioRoom(s, 2)->room.priority == 7,
				"re-tour::scene::SetAudioRoom replaces in place");
			// Swap-and-pop keeps the payload whole.
			tour::scene::SceneAudioRoom room3;
			room3.wet = 0.9f;
			tour::scene::SetAudioRoom(s, 3, room3);
			Check(tour::scene::RemoveAudioRoom(s, 2) && tour::scene::FindAudioRoom(s, 3)
				&& tour::scene::FindAudioRoom(s, 3)->room.wet == 0.9f,
				"audio room swap-and-pop keeps the payload");
			Check(!tour::scene::RemoveAudioRoom(s, 2),
				"audio room re-Remove returns false");
			tour::scene::RemoveAudioRoom(s, 3);
		}
		{
			const AudioSourceRow* ar = FindAudioSource(s, 2);
			Check(ar && ar->source.volume == 0.8f
				&& ar->source.sounds.size() == 1
				&& ar->source.sounds[0].name == "Bells",
				"audio row payload round-trips");
			// Inline-payload replacement in place (string-carrying —
			// the move-not-copy convention).
			SceneAudioSource audio2;
			audio2.volume = 0.5f;
			Check(SetAudioSource(s, 2, audio2)
				&& s.tables.audioSources.size() == 1
				&& FindAudioSource(s, 2)->source.volume == 0.5f
				&& FindAudioSource(s, 2)->source.sounds.empty(),
				"re-SetAudioSource replaces in place");
			// Swap-and-pop keeps the payload whole (the moved row keeps
			// its strings).
			SceneAudioSource audio3;
			audio3.sounds.push_back(AudioSoundRef{ "Wind",
				"content/audio/wind.wav", 0, 0.5f, false });
			SetAudioSource(s, 3, audio3);
			Check(RemoveAudioSource(s, 2) && FindAudioSource(s, 3)
				&& FindAudioSource(s, 3)->source.sounds[0].name == "Wind",
				"audio swap-and-pop keeps the payload");
			Check(!RemoveAudioSource(s, 2), "re-Remove returns false");
			RemoveAudioSource(s, 3);
		}

		// ---- RemoveNodeRows drops every kind at once ----
		Check(RemoveNodeRows(s, 2), "RemoveNodeRows reports removal");
		Check(!FindMesh(s, 2) && !FindMaterial(s, 2) && !FindDecal(s, 2)
			&& !FindCamera(s, 2) && !FindLight(s, 2)
			&& !FindAudioSource(s, 2) && !tour::scene::FindAudioRoom(s, 2),
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

	{
		// ---- Epoch 6b: stable node slots + generations ----
		// (docs/scene_tables_migration.md) Handles survive deletion
		// without renumbering; rows only address LIVE slots; reuse
		// starts clean; the generational NodeHandle guards the ABA case
		// (a stale handle must NOT resurrect across slot reuse).
		Scene s;
		const int root = addNode(s, -1, 0);
		const int a = addNode(s, root, 1);
		const int b = addNode(s, root, 1);
		const int aChild = addNode(s, a, 2);
		SetMesh(s, (uint32_t)a, 5);
		SetLight(s, (uint32_t)b, 6);
		SetDecal(s, (uint32_t)aChild, 7);
		SetNodeUuidRaw(s, (uint32_t)a, "aabbccdd00112233");
		const NodeHandle ha = MakeNodeHandle(s, (uint32_t)a);
		Check(NodeHandleValid(s, ha), "handle valid at mint");

		// ---- delete a subtree: retire, NO renumber ----
		deleteSceneNodes(s, { (uint32_t)a });
		Check(s.hierarchy.size() == 4, "retirement keeps the slot count");
		Check(LiveNodeCount(s) == 2, "live count drops by the subtree");
		Check(!IsNodeAlive(s, (uint32_t)a) && !IsNodeAlive(s, (uint32_t)aChild),
			"the whole subtree retired");
		Check(IsNodeAlive(s, (uint32_t)root) && IsNodeAlive(s, (uint32_t)b),
			"survivors stay alive at their ids");
		Check(FindMesh(s, (uint32_t)a) == nullptr
			&& FindDecal(s, (uint32_t)aChild) == nullptr,
			"rows of retired nodes dropped");
		Check(FindLight(s, (uint32_t)b) && FindLight(s, (uint32_t)b)->light == 6,
			"survivor's row keeps its node id verbatim");
		Check(s.hierarchy[size_t(root)].firstChild == b
			&& s.hierarchy[size_t(b)].nextSibling == -1,
			"survivor's sibling links repaired");
		Check(!NodeHandleValid(s, ha), "handle invalid after retirement");
		Check(MakeNodeHandle(s, (uint32_t)a) == kNodeInvalidHandle,
			"no handle mints for a dead slot");

		// ---- reuse: the freed slots return with bumped generations ----
		// (LIFO: the subtree's highest-retired slot pops first.)
		const int d1 = addNode(s, root, 1);
		Check(d1 == aChild, "addNode reuses freed slots (LIFO free list)");
		const int d = addNode(s, root, 1);
		Check(d == a, "the subtree root's slot returns next");
		Check(NodeHandleValid(s, MakeNodeHandle(s, (uint32_t)d)),
			"the reused slot mints a fresh handle");
		Check(!NodeHandleValid(s, ha),
			"the OLD handle stays invalid across reuse (ABA guard)");
		Check(FindMesh(s, (uint32_t)d) == nullptr,
			"reuse starts row-clean");
		Check(SetMesh(s, (uint32_t)d, 8) && FindMesh(s, (uint32_t)d)->mesh == 8,
			"the reused slot carries new rows");
		Check(GetNodeUuid(s, (uint32_t)d).empty(),
			"reuse clears the old life's metadata");

		// ---- mutators reject retired slots ----
		const int e = addNode(s, root, 1);
		UnlinkNodeFromParent(s, e);
		RetireNodeSlot(s, (uint32_t)e);
		Check(!IsNodeAlive(s, (uint32_t)e), "explicit retire marks dead");
		Check(!SetMesh(s, (uint32_t)e, 9) && !SetLight(s, (uint32_t)e, 9),
			"SetX on a retired slot is rejected");

		// ---- RebuildNodeRows drops rows still pointing at dead slots ----
		// (the discipline backstop — a hand-pushed stale row compacts out.)
		s.tables.meshes.push_back(MeshRow{ (uint32_t)e, 99 });
		RebuildNodeRows(s);
		bool staleRow = false;
		for (const MeshRow& row : s.tables.meshes)
			if (row.node == (uint32_t)e)
				staleRow = true;
		Check(!staleRow, "rebuild compacts rows of retired slots");
	}

	{
		// ---- THE SHARED SPATIAL-VOLUME SHAPE (spatial_volume.h) ----
		// The GI-volume placement convention: center = translation,
		// axes = normalized columns, half = HALF the column lengths.
		// Fades in metres, min-of-six-faces, degenerate shapes cover
		// nothing, and PARITY with the GI volume's own plane math.
		using namespace tour::scene;
		const float inFade = 0.3f, outFade = 0.7f;
		// Axis-aligned 10x4x6 box at (0,2,0): scale = FULL extents.
		const glm::mat4 g = glm::translate(glm::mat4(1.0f),
			glm::vec3(0.0f, 2.0f, 0.0f))
			* glm::scale(glm::mat4(1.0f), glm::vec3(10.0f, 4.0f, 6.0f));
		const SpatialVolumeBox box =
			VolumeBoxFromGlobalTransform(g);
		Check(Near(box.center.y, 2.0f) && Near(box.half.x, 5.0f)
			&& Near(box.half.y, 2.0f) && Near(box.half.z, 3.0f),
			"volume box decodes the GI placement convention");
		Check(Near(SpatialVolumeWeight(box,
				glm::vec3(0.0f, 2.0f, 0.0f), inFade, outFade), 1.0f),
			"volume weight 1 at the center");
		Check(Near(SpatialVolumeWeight(box,
				glm::vec3(20.0f, 2.0f, 0.0f), inFade, outFade), 0.0f),
			"volume weight 0 well outside");
		// The face transition: at the +x face (x = 5) the signed inside
		// distance is 0 -> smoothstep(-outsideFade, insideFade, 0) =
		// t=0.7 -> 0.784 (biased IN, the fade band); a metre past the
		// outer fade (x = 6, d = -1 < -outsideFade) it is 0.
		const float atFace = SpatialVolumeWeight(box,
			glm::vec3(5.0f, 2.0f, 0.0f), inFade, outFade);
		const float pastFace = SpatialVolumeWeight(box,
			glm::vec3(6.0f, 2.0f, 0.0f), inFade, outFade);
		Check(Near(atFace, 0.784f, 1e-3f) && Near(pastFace, 0.0f),
			"volume weight ramps across the face fade");
		// A ROTATED box: rotate 90 degrees about Y — the former +x
		// half-extent becomes the z axis.
		const glm::mat4 gr = glm::translate(glm::mat4(1.0f),
			glm::vec3(0.0f, 2.0f, 0.0f))
			* glm::rotate(glm::mat4(1.0f), glm::radians(90.0f),
				glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::scale(glm::mat4(1.0f), glm::vec3(10.0f, 4.0f, 6.0f));
		const SpatialVolumeBox rb = VolumeBoxFromGlobalTransform(gr);
		Check(Near(rb.half.x, 5.0f) && Near(rb.half.z, 3.0f),
			"rotated box keeps its half extents");
		Check(Near(SpatialVolumeWeight(rb,
				glm::vec3(0.0f, 2.0f, 4.0f), inFade, outFade), 1.0f),
			"rotated box full inside (along the rotated axis)");
		// GI PARITY: the shared weight equals GiVolumeDef::Weight over
		// the same rotated shape (the plane math is the GI volume's
		// own — one convention, two implementations, pinned equal).
		{
			tour::gi::GiVolumeDef def;
			def.planes = tour::gi::GiVolumeBoxPlanesRotated(rb.center,
				rb.axes, rb.half, inFade, outFade);
			bool parity = true;
			for (const glm::vec3 p : {
					glm::vec3(0, 2, 0), glm::vec3(4.9f, 2, 0),
					glm::vec3(5.2f, 2, 0), glm::vec3(6.5f, 2, 0),
					glm::vec3(0, 2.1f, 1.2f), glm::vec3(-3, 3.4f, 2),
					glm::vec3(0, 6.5f, 0), glm::vec3(9, 2, 4) }) {
				if (std::fabs(def.Weight(p)
						- SpatialVolumeWeight(rb, p, inFade, outFade)) > 1e-4f)
					parity = false;
			}
			Check(parity, "spatial volume parity with the GI planes");
		}
			// Degenerate shapes cover nothing (never NaN).
		{
			SpatialVolumeBox flat = box;
			flat.half = glm::vec3(0.0f); // zero extent
			Check(SpatialVolumeWeight(flat,
				glm::vec3(0.0f, 2.0f, 0.0f), inFade, outFade) == 0.0f,
				"degenerate box covers nothing");
			SpatialVolumeBox bad;
			bad.center = glm::vec3(
				std::numeric_limits<float>::quiet_NaN());
			Check(SpatialVolumeWeight(bad,
				glm::vec3(0.0f), inFade, outFade) == 0.0f,
				"non-finite box covers nothing (never NaN)");
		}
	}

	if (g_failures > 0) {
		std::printf("scene_table_tests: %d/%d checks failed\n",
			g_failures, g_checks);
		return 1;
	}
	std::printf("scene_table_tests: all %d checks passed\n", g_checks);
	return 0;
}
