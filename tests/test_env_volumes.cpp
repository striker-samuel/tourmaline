// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// test_env_volumes: Unreal-style env volumes (EnvVolume over the
// transform box). GPU-less: exact 3D coverage (ZoneCovers), priority
// resolution (ZoneAtPosition/ZonePriority), and tour replay of the
// envvol.n keys (parse + validate + look-defaulted priority).

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/tour.h>
#include <Engine/Rendergraph/envzones.h>
#include <Engine/Scene/scene.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	if (++g_checks, cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

// One-node zone: unit box scaled to half extents, placed at origin.
uint32_t MakeZone(::Scene& s, const glm::vec3& half) {
	const int n = addNode(s, -1, 0);
	if (int(s.globalTransform.size()) <= n)
		s.globalTransform.resize(size_t(n) + 1, glm::mat4(1.0f));
	s.globalTransform[size_t(n)] =
		glm::scale(glm::mat4(1.0f), half);
	SceneEnv e{};
	s.envForNode[uint32_t(n)] = e;
	return uint32_t(n);
}

} // namespace

int main() {
	// Half-spaces: box (10,10,10) at origin + single Y faces.
	{
		::Scene s;
		const uint32_t z = MakeZone(s, glm::vec3(10, 10, 10));
		s.envVolumeForNode[z].infMask = kEnvInfNegY; // covers y <= +10
		Check(ZoneCovers(s, z, glm::vec3(0, -100, 0)),
			"envvol.neginf-covers-below");
		Check(!ZoneCovers(s, z, glm::vec3(0, 100, 0)),
			"envvol.neginf-clips-above");
		Check(ZoneCovers(s, z, glm::vec3(5, 0, -5)),
			"envvol.neginf-inside-box");
		Check(!ZoneCovers(s, z, glm::vec3(50, 0, 0)),
			"envvol.neginf-clips-x");
	}
	{
		::Scene s;
		const uint32_t z = MakeZone(s, glm::vec3(10, 2, 10));
		s.envVolumeForNode[z].infMask = kEnvInfPosY; // covers y >= -2
		Check(ZoneCovers(s, z, glm::vec3(0, 100, 0)),
			"envvol.posinf-covers-above");
		Check(!ZoneCovers(s, z, glm::vec3(0, -100, 0)),
			"envvol.posinf-clips-below");
	}
	// All-inf = infinite everywhere; disabled = nothing.
	{
		::Scene s;
		const uint32_t z = MakeZone(s, glm::vec3(10, 10, 10));
		s.envVolumeForNode[z].infMask = 63u;
		Check(ZoneCovers(s, z, glm::vec3(1e6f, -1e6f, 1e6f)),
			"envvol.allinf-covers");
		s.envVolumeForNode[z].enabled = false;
		Check(!ZoneCovers(s, z, glm::vec3(0, 0, 0)),
			"envvol.disabled-covers-nothing");
		Check(ZoneAtPosition(s, glm::vec3(0, 0, 0)) == UINT32_MAX,
			"envvol.disabled-skipped");
	}
	// Legacy INF scale still unbounds the whole axis (regression).
	{
		::Scene s;
		const uint32_t z = MakeZone(s, glm::vec3(10, 10, 10));
		glm::mat4 g = s.globalTransform[z];
		g[1] = glm::vec4(
			std::numeric_limits<float>::infinity(), 0, 0, 0);
		s.globalTransform[z] = g;
		Check(ZoneCovers(s, z, glm::vec3(0, 1e6f, 0)),
			"envvol.legacy-inf-slab");
	}
	// Priority: volume entry beats the look; absent entry falls back.
	{
		::Scene s;
		const uint32_t a = MakeZone(s, glm::vec3(100, 100, 100));
		const uint32_t b = MakeZone(s, glm::vec3(10, 10, 10));
		s.envForNode[a].zonePriority = 5.0f;
		s.envForNode[b].zonePriority = 1.0f;
		s.envVolumeForNode[b].priority = 9.0f;
		Check(ZonePriority(s, a, s.envForNode[a]) == 5.0f,
			"envvol.priority-fallback");
		Check(ZonePriority(s, b, s.envForNode[b]) == 9.0f,
			"envvol.priority-volume-wins");
		Check(ZoneAtPosition(s, glm::vec3(0, 0, 0)) == b,
			"envvol.resolve-volume-priority");
	}
	// Tour replay: envvol.n keys parse, validate, and default priority
	// from the look; dangling nodes drop; volumes-only docs leave the
	// look unauthored.
	{
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetOfflineMode(true);
		const int root = addNode(cluster->scene, -1, 0);
		const int n1 = addNode(cluster->scene, root, 0);
		(void)n1;
		smvk::tour::Doc doc;
		doc.editorSettings.emplace_back("env.n.0.node", "1");
		doc.editorSettings.emplace_back("env.n.0.zonePriority", "5");
		doc.editorSettings.emplace_back("envvol.n.0.node", "1");
		doc.editorSettings.emplace_back("envvol.n.0.mask", "12");
		doc.editorSettings.emplace_back("envvol.n.0.weight", "0.5");
		doc.editorSettings.emplace_back("envvol.n.0.radius", "4");
		doc.editorSettings.emplace_back("envvol.n.0.enabled", "1");
		doc.editorSettings.emplace_back("envvol.n.1.node", "99");
		doc.editorSettings.emplace_back("envvol.n.1.mask", "63");
		std::vector<std::string> warns;
		std::string err;
		Check(smvk::tour::ApplyTourToCluster(*cluster, doc, &warns, &err),
			"envvol.replay-ok");
		const auto it = cluster->scene.envVolumeForNode.find(1);
		Check(it != cluster->scene.envVolumeForNode.end(),
			"envvol.replay-stored");
		if (it != cluster->scene.envVolumeForNode.end()) {
			Check(it->second.infMask == 12u, "envvol.replay-mask");
			Check(it->second.priority == 5.0f,
				"envvol.replay-priority-from-look");
			Check(std::abs(it->second.blendWeight - 0.5f) < 1e-6f,
				"envvol.replay-weight");
			Check(it->second.blendRadius == 4.0f, "envvol.replay-radius");
			Check(it->second.enabled, "envvol.replay-enabled");
		}
		Check(cluster->scene.envVolumeForNode.find(99)
			== cluster->scene.envVolumeForNode.end(),
			"envvol.replay-dangling-dropped");
	}
	{
		// Volumes-only doc: no look blocks, no fake authoring.
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetOfflineMode(true);
		const int root = addNode(cluster->scene, -1, 0);
		const int n1 = addNode(cluster->scene, root, 0);
		(void)n1;
		smvk::tour::Doc doc;
		doc.editorSettings.emplace_back("envvol.n.0.node", "1");
		doc.editorSettings.emplace_back("envvol.n.0.mask", "63");
		doc.editorSettings.emplace_back("envvol.n.0.priority", "7");
		std::vector<std::string> warns;
		std::string err;
		Check(smvk::tour::ApplyTourToCluster(*cluster, doc, &warns, &err),
			"envvol.volsonly-ok");
		const auto it = cluster->scene.envVolumeForNode.find(1);
		Check(it != cluster->scene.envVolumeForNode.end()
			&& it->second.priority == 7.0f,
			"envvol.volsonly-explicit-priority");
		Check(!cluster->scene.envAuthored, "envvol.volsonly-no-fake-author");
	}

	// Scene master switch: the env.zonesEnabled tour key is RETIRED with
	// the always-on rework (75568c82: the default env packs as the infinite
	// base zone - tour.cpp parses the key for format compatibility but
	// forces the flag true). The check pins that contract.
	{
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetOfflineMode(true);
		addNode(cluster->scene, -1, 0);
		smvk::tour::Doc doc;
		doc.editorSettings.emplace_back("env.zonesEnabled", "0");
		std::vector<std::string> warns;
		std::string err;
		Check(smvk::tour::ApplyTourToCluster(*cluster, doc, &warns, &err),
			"envzone.tour-ok");
		Check(cluster->scene.envZonesEnabled,
			"envzone.tour-disable-no-blocks");
	}

	// Analytic zone packer + resolver: exact OBB coverage, rotation,
	// inf-mask slabs, blend falloff and weight-0 skip. Mirrors the shader.
	{
		smvk::envzones::EnvZone look;
		look.skyTint = glm::vec3(1.0f, 0.5f, 0.25f);
		look.ambientI = 7.0f;
		look.fogColor = glm::vec3(0.1f, 0.2f, 0.3f);
		look.fogDensity = 0.02f;
		look.volumetrics = 0.6f;
		look.volumetricDensity = 0.03f;
		smvk::envzones::EnvZoneGpu z;
		Check(smvk::envzones::MakeEnvZoneGpu(
			glm::scale(glm::mat4(1.0f), glm::vec3(10, 2, 10)),
			0u, look, 5.0f, 0.8f, 4.0f, z), "envzone.pack");
		Check(std::abs(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0))
			- 1.0f) < 1e-6f, "envzone.inside");
		Check(std::abs(smvk::envzones::EnvZoneFalloff(z,
			glm::vec3(0, 4, 0)) - 0.5f) < 1e-5f, "envzone.radius-mid");
		Check(smvk::envzones::EnvZoneFalloff(z,
			glm::vec3(0, 6, 0)) == 0.0f, "envzone.radius-out");
		std::vector<smvk::envzones::EnvZoneGpu> zones{ z };
		float w = 0.0f;
		smvk::envzones::EnvZone got;
		Check(smvk::envzones::EnvZoneAt(zones, glm::vec3(0, 4, 0), w, got),
			"envzone.at-hit");
		Check(std::abs(w - 0.4f) < 1e-5f, "envzone.at-weight");
		Check(got.skyTint == glm::vec3(1.0f, 0.5f, 0.25f)
			&& std::abs(got.ambientI - 7.0f) < 1e-6f,
			"envzone.at-look");
		Check(!smvk::envzones::EnvZoneAt(zones, glm::vec3(0, 6, 0), w, got)
			&& w == 0.0f, "envzone.at-outside");
	}

	// Rotated OBB: the long X box turns along Z; inf-mask half-space and
	// a flat finite axis (empty) are handled like ZoneCovers.
	{
		smvk::envzones::EnvZone look;
		smvk::envzones::EnvZoneGpu z;
		const glm::mat4 rot = glm::rotate(glm::mat4(1.0f),
			glm::radians(90.0f), glm::vec3(0, 1, 0));
		Check(smvk::envzones::MakeEnvZoneGpu(rot
			* glm::scale(glm::mat4(1.0f), glm::vec3(10, 1, 1)),
			0u, look, 0.0f, 1.0f, 0.0f, z), "envzone.rot-pack");
		Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0, 0, 9))
			== 1.0f, "envzone.rot-inside");
		Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(9, 0, 0))
			== 0.0f, "envzone.rot-outside");

		Check(smvk::envzones::MakeEnvZoneGpu(
			glm::scale(glm::mat4(1.0f), glm::vec3(10, 10, 10)),
			kEnvInfNegY, look, 0.0f, 1.0f, 0.0f, z),
			"envzone.infmask-pack");
		Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0, -1e6f, 0))
			== 1.0f, "envzone.infmask-below");
		Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0, 1e6f, 0))
			== 0.0f, "envzone.infmask-above");

		Check(!smvk::envzones::MakeEnvZoneGpu(
			glm::scale(glm::mat4(1.0f), glm::vec3(10, 0, 10)),
			0u, look, 0.0f, 1.0f, 0.0f, z), "envzone.flat-empty");
		Check(smvk::envzones::MakeEnvZoneGpu(
			glm::scale(glm::mat4(1.0f), glm::vec3(10, 0, 10)),
			kEnvInfNegY | kEnvInfPosY, look, 0.0f, 1.0f, 0.0f, z),
			"envzone.flat-inf");
		Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0, 1e6f, 0))
			== 1.0f, "envzone.flat-inf-covers");
	}

	// Zero blend weight is skipped (the resolver keeps the plain look).
	{
		smvk::envzones::EnvZone look;
		smvk::envzones::EnvZoneGpu z;
		Check(smvk::envzones::MakeEnvZoneGpu(
			glm::scale(glm::mat4(1.0f), glm::vec3(10, 10, 10)),
			0u, look, 0.0f, 0.0f, 0.0f, z), "envzone.zero-pack");
		std::vector<smvk::envzones::EnvZoneGpu> zones{ z };
		float w = 1.0f;
		smvk::envzones::EnvZone got;
		Check(!smvk::envzones::EnvZoneAt(zones, glm::vec3(0), w, got)
			&& w == 0.0f, "envzone.zero-skip");
	}

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
