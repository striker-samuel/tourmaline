// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// .tourAnim — Tourmaline skeletal animation clips (ASCII, Maya .ma flavor).
//
// A .tourAnim file is a human-readable, versioned list of named animation
// clips: per-joint SPLINE TRACKS of (timestep, value) keys applied on top
// of a source glTF's skeleton. Same contract as .tour: never a blob store,
// the skeleton stays in the source file and is referenced by id only; the
// Editor records clips (live edits + undo) and PakCooker/runtime replay
// them, so what-you-see is what gets cooked:
//
//   // .tourAnim 1 — walk cycle
//   source -p "content/models/robot.gltf" -fingerprint "9f3a...";
//   clip -name "walk" -duration 1.6667 -fps 30;
//
//   track -joint 4 -name "thigh_L" -attr rotate -interp catmullrom;
//     0.0000  0.0000 0.0000 0.0000 1.0000;   // unit quat xyzw
//     0.4167  0.2874 0.1279 0.0231 0.9490;
//     1.6667  0.0000 0.0000 0.0000 1.0000;
//
//   track -joint 4 -attr translate -interp linear;
//     0.0000  0.000 92.100 0.000;
//     1.6667  0.000 92.100 0.000;
//
//   track -joint 7 -attr scale -interp step;
//     0.0000  1.000 1.000 1.000;
//
// A key line is exactly "timestep value": t in seconds from clip start,
// then the channel value — 3 floats for translate/scale (engine units),
// 4 for rotate (UNIT QUATERNION xyzw; the editor inspector's degrees-XYZ
// is a static-transform convention and never appears here).
//
// -joint ids are CLUSTER-space glTF node ids — import-deterministic, the
// same id space .tour verbs address, so the same source always yields the
// same ids on both sides. -name is a recorded joint name, verify-only
// (warn on mismatch, apply by id anyway), mirroring .tour.
//
// Splines (-interp):
//   step        hold the previous key's value
//   linear      lerp; rotate = nlerp shortest-path, renormalized
//   catmullrom  uniform Catmull-Rom through the keys (first/last keys
//               duplicated at the ends); rotate interpolates componentwise
//               then renormalizes — fine at 30 Hz key rates, prefer linear
//               when keys are sparse
//
// Track rules:
//   - key times strictly ascending; a duplicate t drops the earlier key
//     with a warning; sampling clamps before the first / after the last key
//   - a joint without a track keeps its bind-pose local transform
//   - a track replaces the joint's whole channel (cross-fade/additive
//     blending is a runtime sample-time op, not a format concern)
//   - duration shorter than the last key time warns and clamps; fps is
//     informational (authoring rate only)
//
// Unknown verbs, -attr or -interp values warn-and-skip (forward
// compatibility = edit compatibility across versions, same as .tour).

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

struct Scene;

namespace tour::asset {

inline constexpr int kTourAnimVersion = 1;

// Spline through a track's keys (semantics in the header comment).
enum class Spline : uint8_t {
	Step,
	Linear,
	CatmullRom,
};

// Animated joint channel (matches the .tour setAttr verbs / the editor
// inspector: translate / rotate / scale).
enum class Channel : uint8_t {
	Translate,
	Rotate,
	Scale,
};

// One spline key: t seconds from clip start; v = the channel value
// (xyz for Translate/Scale, unit quaternion xyzw for Rotate —
// normalized at load, sign-fixed to its predecessor for shortest path).
struct Key {
	double t = 0.0;
	glm::vec4 v{ 0.0f };
};

// Per-joint spline track; keys ascending in t (load-enforced).
struct Track {
	uint32_t joint = 0;   // cluster-space joint (glTF node) id
	Channel channel = Channel::Translate;
	Spline spline = Spline::Linear;
	std::vector<Key> keys;
	std::string name;     // recorded joint name (verify-only, like .tour)
};

// Named clip. One file may carry a set (idle/walk/run); each `clip` verb
// starts a new one and owns every `track` until the next.
struct Clip {
	std::string name;
	double duration = 0.0; // seconds, <= last key time (warn + clamp)
	double fps = 30.0;     // authoring rate, informational only
	std::vector<Track> tracks;
};

struct AnimDoc {
	int version = kTourAnimVersion;
	std::string source;      // source glTF path (as written by the editor)
	std::string fingerprint; // size+mtime of source at save (warn on mismatch)
	std::vector<Clip> clips;
};

bool SaveTourAnim(const std::string& path, const AnimDoc& doc, std::string& err);
bool LoadTourAnim(const std::string& path, AnimDoc& doc, std::string& err,
	std::vector<std::string>* warnings = nullptr);
// Text-form loader (the same grammar from an in-memory string - the
// editor/tests read payloads they already hold; the file loader wraps
// this). Same warn-and-skip forward compatibility as .tour.
bool LoadTourAnimText(const std::string& text, AnimDoc& doc, std::string& err,
	std::vector<std::string>* warnings = nullptr);

// ---------------------------------------------------------------------------
// Runtime skeletal model
// ---------------------------------------------------------------------------

// Skeleton cooked from a glTF skin: joints[i] is the cluster node id,
// inverseBind[i] its inverse world matrix at bind time. PackedSkinInfluences joint
// ids (mesh.h) INDEX this table (glTF JOINTS_0 order); .tourAnim -joint
// ids are node ids — map track -> palette index once at bind time.
struct Skeleton {
	std::vector<uint32_t> joints;       // cluster node ids, pool order
	std::vector<glm::mat4> inverseBind; // world^-1 at bind pose
	uint32_t root = 0;                  // skeleton root node id

	uint32_t Count() const { return uint32_t(joints.size()); }
};

// Sample one track at t seconds (clamped to the key range). Returns xyz
// for Translate/Scale (w unused), a unit quaternion xyzw for Rotate.
// Untracked joints/channels keep their bind-pose locals (caller's job).
glm::vec4 SampleTrack(const Track& track, double t);

// Build the skin palette the skinned vertex shader fetches:
// palette[i] = scene.globalTransform[joints[i]] * inverseBind[i].
// The pose pass writes sampled joint locals into scene.localTransform
// (composed as T * R * S) before the scene's regular transform update,
// then this rides the existing global-transform walk (see scene.h).
void BuildSkinPalette(const Scene& scene, const Skeleton& skeleton,
	std::vector<glm::mat4>& palette);

} // namespace tour::asset

// COMPAT (the namespace/RHI boundary audit): the .tourAnim model is
// ENGINE-OWNED and lives in tour::asset (with the .tour model). The
// namespace alias keeps every existing smvk::touranim:: spelling
// resolving.
namespace smvk {
	namespace touranim = ::tour::asset;
}