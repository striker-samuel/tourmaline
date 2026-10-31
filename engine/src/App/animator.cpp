// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The animator engine — see animator.h. Pure TRS-channel sampling +
// blending (no world access): the layers replace whole channels over
// the REST pose, the crossfade blends the layered results, and the
// composed T*R*S locals land in state.jointLocals (skeleton joint
// order) for the World flush.

#include <Engine/App/animator.h>

#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Engine/App/world.h>

#include <algorithm>
#include <cmath>

namespace smvk {

namespace {

// Decompose a local into TRS (the Cluster::SampleAnimation convention:
// exact round trip for TRS-authored locals; a sheared local keeps its
// shear dropped — the import path never produces those).
struct LocalTRS {
	glm::vec3 t{ 0.0f };
	glm::quat r{ 1.0f, 0.0f, 0.0f, 0.0f };
	glm::vec3 s{ 1.0f };
};

LocalTRS DecomposeLocal(const glm::mat4& m) {
	LocalTRS out;
	glm::vec3 skew;
	glm::vec4 persp;
	glm::decompose(m, out.s, out.r, out.t, skew, persp);
	return out;
}

glm::mat4 ComposeLocal(const LocalTRS& l) {
	glm::mat4 m(1.0f);
	m = glm::translate(m, l.t);
	m *= glm::mat4_cast(l.r);
	m = glm::scale(m, l.s);
	return m;
}

// Samples one layer into `out` (joint order = the skeleton's), REPLACing
// whole channels over the rest pose — the .tourAnim rule: a track owns
// its joint's channel, untracked joints/channels keep the rest.
void SampleLayer(const AnimatorLayer& layer,
	const std::vector<glm::mat4>& restLocals,
	std::vector<LocalTRS>& out) {
	// Start from the rest (the per-joint base).
	for (size_t i = 0; i < out.size(); ++i)
		out[i] = DecomposeLocal(
			i < restLocals.size() ? restLocals[i] : glm::mat4(1.0f));
	if (!layer.active)
		return;
	const double t = layer.cursor;
	for (const touranim::Track& track : layer.clip.tracks) {
		if (track.keys.empty() || track.joint >= out.size())
			continue;
		const glm::vec4 v = touranim::SampleTrack(track, t);
		switch (track.channel) {
		case touranim::Channel::Translate:
			out[track.joint].t = glm::vec3(v.x, v.y, v.z);
			break;
		case touranim::Channel::Scale:
			out[track.joint].s = glm::vec3(v.x, v.y, v.z);
			break;
		case touranim::Channel::Rotate:
			// nlerp-normalized at sample time (the keys are unit +
			// sign-fixed; the spline output stays on the unit sphere
			// to float precision).
			{
				glm::quat q(v.w, v.x, v.y, v.z);
				const float ql = std::sqrt(
					q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
				if (ql > 1e-8f)
					q /= ql;
				out[track.joint].r = q;
			}
			break;
		}
	}
}

// Advances one layer's cursor (loop wraps; a finished one-shot
// retires so its weight stops contributing).
void AdvanceLayer(AnimatorLayer& layer, float dt) {
	if (!layer.active)
		return;
	layer.cursor += double(dt);
	if (layer.clip.duration > 1e-6) {
		if (layer.loop) {
			while (layer.cursor >= layer.clip.duration)
				layer.cursor -= layer.clip.duration;
		}
		else if (layer.cursor >= layer.clip.duration) {
			layer.cursor = layer.clip.duration; // clamp at the last pose
			layer.active = false;
		}
	}
}

} // namespace

void AdvanceAnimator(AnimatorState& state, float dt) {
	if (!state.playing || state.paused)
		return;
	const float scaled = dt * state.speed;
	// The crossfade clock (the fade's weight rides it).
	if (state.fading) {
		state.fadeT += scaled;
		if (state.fadeDuration > 1e-6f)
			state.fade.weight =
				std::clamp(state.fadeT / state.fadeDuration, 0.0f, 1.0f);
		if (state.fadeT >= state.fadeDuration) {
			// Fade complete: the target becomes the current (its loop
			// flag, its cursor mid-flight).
			state.current = state.fade;
			state.current.weight = 1.0f;
			state.fade = AnimatorLayer{};
			state.fading = false;
		}
	}
	AdvanceLayer(state.current, scaled);
	AdvanceLayer(state.fade, scaled);
	// A fully-retired animator (one-shot finished, no fade) stops.
	if (!state.current.active && !state.fading)
		state.playing = false;

	// Evaluate: sample both layers over the rest, blend by weight.
	const size_t jointCount = state.restLocals.size();
	std::vector<LocalTRS> a(jointCount), b(jointCount);
	SampleLayer(state.current, state.restLocals, a);
	SampleLayer(state.fade, state.restLocals, b);
	// The blend weights: during a fade the CURRENT ramps DOWN as the
	// target ramps up (the target's weight is the ramp); outside a
	// fade the current owns the pose.
	const float wFade = state.fading ? state.fade.weight : 0.0f;
	const float wCur = 1.0f - wFade;
	state.jointLocals.resize(jointCount);
	for (size_t i = 0; i < jointCount; ++i) {
		if (wFade <= 0.0f) {
			state.jointLocals[i] = ComposeLocal(a[i]);
			continue;
		}
		LocalTRS blended;
		blended.t = wCur * a[i].t + wFade * b[i].t;
		blended.s = wCur * a[i].s + wFade * b[i].s;
		// Shortest-path nlerp (the sign fix at load keeps the arc
		// minor; dot < 0 flips the target's sign for the blend).
		glm::quat q0 = a[i].r, q1 = b[i].r;
		if (glm::dot(q0, q1) < 0.0f)
			q1 = -q1;
		glm::quat q = wCur * q0 + wFade * q1;
		const float ql = std::sqrt(
			q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
		blended.r = ql > 1e-8f ? q / ql : q0;
		state.jointLocals[i] = ComposeLocal(blended);
	}
}

void AnimatorPlay(AnimatorState& state, touranim::Clip clip, bool loop) {
	state.current.clip = std::move(clip);
	state.current.cursor = 0.0;
	state.current.weight = 1.0f;
	state.current.loop = loop;
	state.current.active = true;
	// A play mid-fade replaces the whole stack.
	state.fade = AnimatorLayer{};
	state.fading = false;
	state.fadeT = 0.0f;
	state.playing = true;
}

void AnimatorCrossFade(AnimatorState& state, touranim::Clip clip,
	float fadeSeconds) {
	if (fadeSeconds <= 1e-6f || !state.current.active) {
		AnimatorPlay(state, std::move(clip), true);
		return;
	}
	state.fade.clip = std::move(clip);
	state.fade.cursor = 0.0;
	state.fade.weight = 0.0f;
	state.fade.loop = true;
	state.fade.active = true;
	state.fadeDuration = fadeSeconds;
	state.fadeT = 0.0f;
	state.fading = true;
	state.playing = true;
}

// ---- the facade (every operation validates the handle + binding) ----

Animator::operator bool() const {
	return m_world && m_handle && m_world->IsAnimatorAlive(m_handle);
}

void Animator::Play(touranim::Clip clip, bool loop) {
	if (AnimatorState* s =
			m_world ? m_world->AnimatorStateOf(m_handle) : nullptr)
		AnimatorPlay(*s, std::move(clip), loop);
}

void Animator::CrossFade(touranim::Clip clip, float fadeSeconds) {
	if (AnimatorState* s =
			m_world ? m_world->AnimatorStateOf(m_handle) : nullptr)
		AnimatorCrossFade(*s, std::move(clip), fadeSeconds);
}

void Animator::SetSpeed(float speed) {
	if (AnimatorState* s =
			m_world ? m_world->AnimatorStateOf(m_handle) : nullptr)
		s->speed = std::max(speed, 0.0f);
}

void Animator::Pause() {
	if (AnimatorState* s =
			m_world ? m_world->AnimatorStateOf(m_handle) : nullptr)
		s->paused = true;
}

void Animator::Resume() {
	if (AnimatorState* s =
			m_world ? m_world->AnimatorStateOf(m_handle) : nullptr)
		s->paused = false;
}

void Animator::Stop() {
	if (AnimatorState* s =
			m_world ? m_world->AnimatorStateOf(m_handle) : nullptr) {
		s->playing = false;
		s->paused = false;
		// A stop mid-fade drops the target (the current clip's last
		// pose stands until the next Play).
		s->fade = AnimatorLayer{};
		s->fading = false;
	}
}

float Animator::Speed() const {
	const AnimatorState* s =
		m_world ? m_world->AnimatorStateOf(m_handle) : nullptr;
	return s ? s->speed : 0.0f;
}

bool Animator::Playing() const {
	const AnimatorState* s =
		m_world ? m_world->AnimatorStateOf(m_handle) : nullptr;
	return s && s->playing && !s->paused;
}

} // namespace smvk
