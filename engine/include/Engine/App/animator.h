// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// The ANIMATOR COMPONENT (the entity model's Option B, docs/
// audio_source.md's handle section + the runtime asset/entity design):
// explicit per-entity playback state the WORLD owns — the animator is
// not an asset and owns neither the skeleton nor the clips' shared
// data. Gameplay code composes the handle:
//
//   struct Knight {
//       MeshEntityHandle mesh{};
//       AnimatorHandle animator{};
//   };
//   knight.mesh = world.SpawnMeshFromNode(knightRig, frame, parent);
//   auto anim = world.AddAnimator(knight.mesh, std::move(idleClip));
//   anim.CrossFade(std::move(walkClip), 0.25f);
//   anim.SetSpeed(1.5f);
//
// EVALUATION MODEL: each animator keeps its pose in a PRIVATE joint
// array (rest locals captured at attach + track samples REPLACING
// whole channels, the .tourAnim rule). The skeleton binding is the
// entity mesh's OWNING cluster (AddAnimator validates hasSkeleton —
// the v1 single-skeleton-per-cluster contract). World::Update(dt)
// advances every animator and flushes the FIRST-ALIVE animator's pose
// into its target cluster's scene (the cluster-wide pose the existing
// skinning path consumes; Cluster::poseExternallyDriven tells the
// renderer the cluster is animator-driven — it stops its own clip
// advance). Independent playback STATE for every entity is exact;
// independent RENDERING is the v1 limit (one pose per cluster — the
// per-draw palette table is the documented renderer follow-up).
//
// CLIPS are touranim::Clip values (loaded through the .tourAnim
// channel — LoadTourAnimText; the AnimationClipKey catalog bucket +
// the LoadAnimation asset surface are the follow-up that makes
// `anim.Play(project::animations::Idle)` resolve bytes by id). Clips
// pass BY VALUE: the animator owns its copy (shared clip data caches
// are the asset manager's business, not the component's).

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Engine/Asset/touranim.h> // Clip/Track/Skeleton
#include <Engine/Scene/entity_handle.h>

namespace smvk {

struct World;

// One playing LAYER (an animator carries at most two: the current clip
// + the crossfade target). `cursor` advances by dt*speed (looping
// wraps at the clip duration; one-shots clamp and retire).
struct AnimatorLayer {
	touranim::Clip clip;
	double cursor = 0.0;
	float weight = 1.0f; // the blend weight (crossfade ramps it)
	bool loop = true;
	bool active = false;
};

// World-owned playback state (one per AddAnimator; the registry's
// slot carries the generational identity — see AnimatorHandle).
// `restLocals` is the pose captured at ATTACH (a fresh cluster's
// loaded locals = the bind pose; document: attach before any other
// writer moves the joints if you need exact bind).
struct AnimatorState {
	AnimatorLayer current;
	AnimatorLayer fade;        // the crossfade target
	float fadeDuration = 0.0f; // seconds; 0 = snap
	float fadeT = 0.0f;        // elapsed fade time
	bool fading = false;
	float speed = 1.0f;
	bool paused = false;
	bool playing = false;
	// The binding: the mesh entity + its skeleton's owning cluster +
	// the captured rest pose (per skeleton joint, cluster node order).
	MeshEntityHandle mesh{};
	Cluster* target = nullptr;
	std::vector<glm::mat4> restLocals;
	// The evaluated pose (joint LOCALS, skeleton joint order — the
	// flush writes these into the target cluster's scene).
	std::vector<glm::mat4> jointLocals;
};

// Advances the state by dt (no world access — pure math) and evaluates
// the blended pose into state.jointLocals: each layer samples its
// tracks (untracked joints/channels keep the REST pose — the .tourAnim
// rule), the crossfade blends the two layers' TRS channels
// (translate/scale lerp, rotate nlerp — shortest-path, the keys are
// sign-fixed at load). Paused/stopped animators keep their last pose.
void AdvanceAnimator(AnimatorState& state, float dt);

// Starts/restarts a clip (loop = wrap at duration). A running clip is
// replaced (the classic Play semantics — no fade).
void AnimatorPlay(AnimatorState& state, touranim::Clip clip, bool loop);
// Cross-fades to a clip over fadeSeconds (the running clip's weight
// ramps down as the target's ramps up; the fade completes by
// replacement). fadeSeconds <= 0 = an immediate Play.
void AnimatorCrossFade(AnimatorState& state, touranim::Clip clip,
	float fadeSeconds);

// The lightweight FACADE over World-owned state (the public API the
// gameplay examples drive — every operation validates the handle and
// the binding). Default-constructed reads false; a destroyed animator
// (or its mesh) fails every call silently.
class Animator {
public:
	Animator() = default;
	Animator(World& world, AnimatorHandle handle)
		: m_world(&world), m_handle(handle) {}

	[[nodiscard]] explicit operator bool() const;
	void Play(touranim::Clip clip, bool loop = true);
	void CrossFade(touranim::Clip clip, float fadeSeconds);
	void SetSpeed(float speed);
	void Pause();
	void Resume();
	void Stop();
	[[nodiscard]] float Speed() const;
	[[nodiscard]] bool Playing() const;

	[[nodiscard]] AnimatorHandle handle() const { return m_handle; }

private:
	World* m_world = nullptr;
	AnimatorHandle m_handle{};
};

} // namespace smvk
