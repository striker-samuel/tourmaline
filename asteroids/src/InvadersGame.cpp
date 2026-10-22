// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Crude SPACE INVADERS in the sponza atrium (see InvadersGame.h for the
// architecture). Everything here is the app's own content: the engine
// never knows a game exists — the balls are ordinary world mesh nodes.

#include "InvadersGame.h"

#include <Engine/App/cluster.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

namespace app {
	namespace {

		// Lat-long UV sphere — the chrome-ball recipe from PakApp.cpp
		// (same CCW winding: dTheta x dPhi points outward). White vertex
		// color; the material owns the tint through baseColor/emissive.
		smvk::StaticMeshUpload BuildBallUpload(float radius, int slices, int stacks) {
			smvk::StaticMeshUpload data;
			data.aabbMin = glm::vec3(-radius);
			data.aabbMax = glm::vec3(radius);
			const float pi = glm::pi<float>();
			const int nSlices = std::max(8, slices);
			const int nStacks = std::max(4, stacks);
			for (int j = 0; j <= nStacks; ++j) {
				const float phi = pi * static_cast<float>(j) / static_cast<float>(nStacks);
				const float sinPhi = std::sin(phi);
				const float cosPhi = std::cos(phi);
				for (int i = 0; i <= nSlices; ++i) {
					const float theta = 2.0f * pi * static_cast<float>(i) / static_cast<float>(nSlices);
					const float sinT = std::sin(theta);
					const float cosT = std::cos(theta);
					const glm::vec3 n(sinPhi * cosT, cosPhi, sinPhi * sinT);
					smvk::StaticVertex v{};
					v.pos = n * radius;
					v.color = glm::vec4(1.0f);
					v.texCoord = glm::vec2(
						static_cast<float>(i) / static_cast<float>(nSlices),
						static_cast<float>(j) / static_cast<float>(nStacks));
					v.normal = n;
					glm::vec3 t(-sinT, 0.0f, cosT);
					if (glm::dot(t, t) < 1e-12f)
						t = glm::vec3(1.0f, 0.0f, 0.0f); // poles: theta tangent degenerates
					v.tangent = glm::vec4(glm::normalize(t), 1.0f);
					data.vertices.push_back(v);
				}
			}
			const uint32_t row = static_cast<uint32_t>(nSlices) + 1u;
			for (int j = 0; j < nStacks; ++j) {
				for (int i = 0; i < nSlices; ++i) {
					const uint32_t a = static_cast<uint32_t>(j) * row + static_cast<uint32_t>(i);
					const uint32_t b = a + 1u;
					const uint32_t c = a + row + 1u;
					const uint32_t d = a + row;
					data.indices.push_back(a);
					data.indices.push_back(b);
					data.indices.push_back(c);
					data.indices.push_back(a);
					data.indices.push_back(c);
					data.indices.push_back(d);
				}
			}
			return data;
		}

		// ---- field layout (sponza atrium: floor y=0, origin = centre) ----
		// ROTATED 90 deg vs the nave: the enemy->player ADVANCE axis runs
		// along the corridor's long axis (X, the walkable atrium strip x in
		// [-3.8, 4.8]) and the strafe axis runs across the corridor's OPEN
		// width (z in [-1.2, 2.6], walls at z ~ -2.3/+2.6 — see the sponza
		// GI volumes in Sponza.tour). The first cut ran the field along Z and
		// poked past both corridor walls: the player row at z=+3.1 sat under
		// the gallery structure and the ball never showed from the top-down
		// camera; this layout keeps every ball inside the open hall by
		// construction. Screen-up is world -X (the camera up hint below), so
		// the player row at +X is screen BOTTOM, the invaders march "down"
		// the nave toward it, and screen-right = world -Z.
		constexpr float kPlayerRadius = 0.42f;
		constexpr float kEnemyRadius = 0.30f;
		constexpr float kBulletRadius = 0.11f;
		constexpr float kPlayerX = 3.3f;         // player row (+X = screen bottom)
		constexpr float kPlayerZMin = -0.7f;     // strafe clamp across the corridor
		constexpr float kPlayerZMax = 2.1f;
		constexpr float kPlayerSpeed = 5.0f;
		constexpr float kPlayerFireCooldown = 0.34f;
		constexpr float kPlayerBulletSpeed = 10.5f;    // -X (screen up, down the nave)
		constexpr float kEnemyBulletSpeedBase = 3.0f;  // +X (screen down; dodgeable)
		constexpr float kEnemyBulletSpeedPerWave = 0.3f;
		constexpr float kBulletTopX = -3.4f;           // player bullets die past the top
		constexpr float kBulletBottomX = 4.2f;         // enemy bullets die past the bottom
		// Invader grid: 5 columns across the corridor width (the class's
		// kEnemyCols — the array shape), 3 rows stacked up the nave. The
		// formation marches as one: per-frame z deltas shift every alive
		// ball, a bounce advances x toward the player.
		constexpr float kEnemyColStep = 0.72f;
		constexpr float kEnemyRowStep = 0.95f;
		constexpr float kEnemyFrontX = -0.8f;    // front row (closest to the player)
		// Formation centre: biased NORTH of the corridor midline — the south
		// arch wall sits at z ~ +2.6 and the grid half-span is 1.44 + ball
		// 0.30 + march 0.22, so z=0.4 tops out at 2.36 (clear by ~0.25 m);
		// the north reach (-1.56) stays clear of the aisle arches (~-2.0).
		constexpr float kEnemyCenterZ = 0.4f;
		constexpr float kFormationZLimit = 0.22f;  // |strafe offset| clamp (walls)
		constexpr float kFormationAdvance = 0.09f; // +X step per bounce (nave march)
		constexpr float kBreachX = 2.7f;           // invaders reaching the player row
		// Fixed top-down camera: straight down at the corridor centre of the
		// atrium, high enough that the scene's authored fov (~55 deg measured)
		// covers the whole field from inside the domed hall (11.5 m tall —
		// the camera at 8.6 m is clear air). Up hint (-1,0,0) rotates the
		// screen basis 90 deg vs the nave: screen-up = world -X.
		const glm::vec3 kCamEye(0.3f, 8.6f, 0.4f);
		const glm::vec3 kCamTarget(0.3f, 0.0f, 0.4f);
		const glm::vec3 kCamUp(-1.0f, 0.0f, 0.0f);
		// Draw flags for every ball node — the resolve flag recipe for an
		// Opaque lit caster (mirrored from World::ResolveAndFixup_clusters
		// / the editor's RefreshNodeDrawState).
		const smvk::IteratorFlags kBallDrawFlags =
			smvk::IteratorFlags(smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_StaticVertexFmt
				| smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_CastShadow);

	} // namespace

	InvadersElement::InvadersElement(smvk::App& host,
		engine::WorldRenderer& renderer,
		std::shared_ptr<smvk::World*> worldHandle)
		: m_host(host)
		, m_input(host.m_input)
		, m_renderer(renderer)
		, m_worldHandle(std::move(worldHandle)) {
	}

	bool InvadersElement::onAttach() {
		m_world = m_worldHandle ? *m_worldHandle : nullptr;
		if (!m_world || m_world->clusters.empty()) {
			std::cerr << "[Invaders] no world to play in - game off\n";
			return true; // benign: the viewer keeps running
		}
		smvk::ResourceManager* ctx = m_host.GetResourceManager();
		if (!ctx) {
			std::cerr << "[Invaders] no resource manager - game off\n";
			return true;
		}
		smvk::Cluster& dyn = m_world->EnsureDynamicCluster(ctx);
		if (!dyn.ctx_)
			dyn.ctx_ = ctx; // a value-only dynamic cluster (decals) predating us

		// Ball materials: bright unlit-ish PBR with a modest emissive so
		// the balls pop against the atrium floor without nuking bloom.
		auto makeMat = [&](const glm::vec4& base, const glm::vec3& emissive,
			float strength, float roughness) -> uint32_t {
			smvk::MaterialCreateInfo ci{};
			ci.baseColorFactor = base;
			ci.emissiveFactor = emissive;
			ci.emissiveStrength = strength;
			ci.metallicFactor = 0.0f;
			ci.roughnessFactor = roughness;
			ci.alphaMode = smvk::AlphaMode::Opaque;
			ci.shadingModel = smvk::ShadingModel::DefaultLit;
			smvk::ClusterMaterialHandle h = dyn.CreateMaterial(ci);
			if (h.empty()) {
				smvk::OnResourceExhaustion("InvadersElement.material");
				return UINT32_MAX;
			}
			const uint32_t idx = uint32_t(m_world->materialLocations.size());
			m_world->materialLocations.push_back({ &dyn, h });
			return idx;
		};
		m_matPlayer = makeMat(
			glm::vec4(0.05f, 0.85f, 0.12f, 1.0f),
			glm::vec3(0.10f, 1.10f, 0.18f), 1.6f, 0.35f);
		m_matEnemy = makeMat(
			glm::vec4(0.85f, 0.06f, 0.04f, 1.0f),
			glm::vec3(1.00f, 0.08f, 0.05f), 1.2f, 0.40f);
		m_matBulletPlayer = makeMat(
			glm::vec4(1.00f, 0.95f, 0.60f, 1.0f),
			glm::vec3(4.00f, 3.60f, 2.00f), 1.0f, 0.30f);
		m_matBulletEnemy = makeMat(
			glm::vec4(1.00f, 0.30f, 0.10f, 1.0f),
			glm::vec3(3.20f, 0.70f, 0.20f), 1.0f, 0.30f);
		if (m_matPlayer == UINT32_MAX || m_matEnemy == UINT32_MAX
			|| m_matBulletPlayer == UINT32_MAX || m_matBulletEnemy == UINT32_MAX) {
			std::cerr << "[Invaders] material creation failed - game off\n";
			return true;
		}

		// Ball entities: the player, the invader grid, the bullet pools.
		// Tessellation stays crude (they are small on screen).
		if (!CreateBall(m_player, kPlayerRadius, 28, 20, m_matPlayer, "invaders_player")) {
			std::cerr << "[Invaders] player ball failed - game off\n";
			return true;
		}
		for (uint32_t i = 0; i < m_enemies.size(); ++i) {
			char name[32];
			std::snprintf(name, sizeof(name), "invaders_enemy_%u", i);
			if (!CreateBall(m_enemies[i], kEnemyRadius, 22, 16, m_matEnemy, name)) {
				std::cerr << "[Invaders] enemy ball " << i << " failed - game off\n";
				return true;
			}
		}
		for (uint32_t i = 0; i < m_playerBullets.size(); ++i) {
			char name[32];
			std::snprintf(name, sizeof(name), "invaders_pbullet_%u", i);
			if (!CreateBall(m_playerBullets[i], kBulletRadius, 10, 8,
				m_matBulletPlayer, name)) {
				std::cerr << "[Invaders] player bullet " << i << " failed - game off\n";
				return true;
			}
		}
		for (uint32_t i = 0; i < m_enemyBullets.size(); ++i) {
			char name[32];
			std::snprintf(name, sizeof(name), "invaders_ebullet_%u", i);
			if (!CreateBall(m_enemyBullets[i], kBulletRadius, 10, 8,
				m_matBulletEnemy, name)) {
				std::cerr << "[Invaders] enemy bullet " << i << " failed - game off\n";
				return true;
			}
		}

		// Fixed top-down framing: straight down with the up hint ROTATED 90
		// deg vs the nave (screen-up = world -X, screen-right = world -Z),
		// so the invader grid stacks up the corridor and marches down the
		// screen toward the player. The hint is perpendicular to the view
		// direction, so ResolveLookAtUp passes it through unchanged.
		m_renderer.CameraPositioner().lookAt(kCamEye, kCamTarget,
			ResolveLookAtUp(kCamEye, kCamTarget, kCamUp));

		ResetGame();
		m_attached = true;
		m_renderer.SetExtraUi([this]() { DrawHud(); });
		std::cerr << "[Invaders] on: 1 green ball vs " << m_enemies.size()
			<< " red balls, top-down camera at (" << kCamEye.x << ","
			<< kCamEye.y << "," << kCamEye.z << ")\n";
		return true;
	}

	void InvadersElement::onDetach() {
		// Drop the HUD hook first (the renderer outlives this element in
		// the detach pass — reverse stack order). Nothing else to free:
		// the dynamic cluster + the world own every mesh/material/node.
		m_renderer.SetExtraUi({});
		m_attached = false;
	}

	bool InvadersElement::CreateBall(Ball& b, float radius, int slices, int stacks,
		uint32_t matIdx, const char* name) {
		if (!m_world || matIdx == UINT32_MAX)
			return false;
		smvk::World& w = *m_world;
		smvk::ResourceManager* ctx = m_host.GetResourceManager();
		smvk::Cluster& dyn = w.EnsureDynamicCluster(ctx);
		smvk::ClusterStaticMeshHandle mesh =
			dyn.UploadStaticMesh(BuildBallUpload(radius, slices, stacks));
		if (mesh.empty()) {
			smvk::OnResourceExhaustion("InvadersElement.CreateBall");
			return false;
		}
		// World pool row (the draw list + UpdateMaterials read these).
		b.meshIdx = uint32_t(w.meshLocations.size());
		w.meshLocations.push_back({ &dyn, mesh });
		b.matIdx = matIdx;
		b.radius = radius;
		// Merged-scene root node (stable id: the scene only grows).
		const int node = addNode(w.scene, -1, 0);
		if (node < 0)
			return false;
		b.node = uint32_t(node);
		// Parked BELOW the floor until shown: hidden balls carry no mesh
		// link, but keep a sane local so nothing can flash them at the
		// origin (the chrome ball's spot).
		const glm::mat4 park =
			glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -50.0f, 0.0f));
		w.scene.localTransform[size_t(node)] = park;
		w.scene.globalTransform[size_t(node)] = park;
		if (name)
			setNodeName(w.scene, node, name);
		return true;
	}

	void InvadersElement::ShowBall(Ball& b) {
		if (!b.alive || b.node == Ball::kNoNode || !m_world)
			return;
		smvk::World& w = *m_world;
		Scene& s = w.scene;
		if (s.meshForNode.contains(b.node))
			return; // already visible
		s.meshForNode[b.node] = b.meshIdx;
		s.materialForNode[b.node] = b.matIdx;
		if (w.flags.size() <= size_t(b.node))
			w.flags.resize(size_t(b.node) + 1, 0);
		w.flags[b.node] = kBallDrawFlags;
		w.drawItems.push_back(smvk::World::RenderItem{
			b.node, b.meshIdx, b.matIdx, kBallDrawFlags });
		w.itemFlags.push_back(kBallDrawFlags);
		w.flagLists.clear(); // cooked flag index lists are stale now
	}

	void InvadersElement::HideBall(Ball& b) {
		if (b.node == Ball::kNoNode || !m_world)
			return;
		smvk::World& w = *m_world;
		Scene& s = w.scene;
		s.meshForNode.erase(b.node);
		s.materialForNode.erase(b.node);
		if (size_t(b.node) < w.flags.size())
			w.flags[b.node] = 0;
		for (size_t i = w.drawItems.size(); i-- > 0;) {
			if (w.drawItems[i].node == b.node) {
				w.drawItems.erase(w.drawItems.begin() + ptrdiff_t(i));
				w.itemFlags.erase(w.itemFlags.begin() + ptrdiff_t(i));
				break;
			}
		}
		w.flagLists.clear();
	}

	void InvadersElement::SetBallPos(Ball& b, const glm::vec3& p) {
		b.pos = p;
		if (b.node == Ball::kNoNode || !m_world)
			return;
		Scene& s = m_world->scene;
		s.localTransform[b.node] = glm::translate(glm::mat4(1.0f), p);
		// The FULL recalc (not the Dynamic variant): our nodes are the
		// only dirty roots, and the dynamic variant folds just one
		// level-0 entry per call (the editor's live path uses this same
		// pair). The actual recalculate runs once at the frame tail.
		markAsChanged(s, int(b.node));
	}

	void InvadersElement::ResetGame() {
		if (!m_world)
			return;
		m_score = 0;
		m_lives = 3;
		m_wave = 1;
		m_phase = Phase::Playing;
		m_phaseTimer = 0.0f;
		m_invulnTimer = 1.5f; // spawn grace
		m_rng = 0x9E3779B9u;
		for (Ball& b : m_playerBullets) {
			b.alive = false;
			HideBall(b);
		}
		for (Ball& b : m_enemyBullets) {
			b.alive = false;
			HideBall(b);
		}
		StartWave();
		m_player.alive = true;
		SetBallPos(m_player, glm::vec3(kPlayerX, kPlayerRadius, kEnemyCenterZ));
		ShowBall(m_player);
		recalculateGlobalTransforms(m_world->scene);
	}

	void InvadersElement::StartWave() {
		m_formation = glm::vec2(0.0f);
		m_enemyDir = 1.0f;
		m_enemyFireCooldown = 1.6f;
		m_fireCooldown = 0.0f;
		uint32_t i = 0;
		for (uint32_t r = 0; r < kEnemyRows; ++r) {
			for (uint32_t c = 0; c < kEnemyCols; ++c, ++i) {
				Ball& e = m_enemies[i];
				e.alive = true;
				// Columns across the corridor (Z), rows up the nave (X):
				// row 0 is the FRONT (closest to the player, largest x).
				e.pos = glm::vec3(
					kEnemyFrontX - float(r) * kEnemyRowStep,
					kEnemyRadius,
					kEnemyCenterZ
						+ (float(c) - float(kEnemyCols - 1) * 0.5f) * kEnemyColStep);
				SetBallPos(e, e.pos);
				ShowBall(e);
			}
		}
	}

	void InvadersElement::FirePlayerBullet() {
		for (Ball& b : m_playerBullets) {
			if (b.alive)
				continue;
			b.alive = true;
			SetBallPos(b, m_player.pos
				+ glm::vec3(-(m_player.radius + kBulletRadius + 0.05f), 0.0f, 0.0f));
			ShowBall(b);
			// Muzzle pop (tiny warm spark at the launch point).
			m_renderer.SpawnParticleBurst(b.pos,
				glm::vec3(1.0f, 0.85f, 0.45f), 1.2f, 0.014f, 0.25f, 10);
			return;
		}
	}

	void InvadersElement::FireEnemyBullet() {
		// Shooter: the front-most (closest to the player, max x) alive
		// invader of a random OCCUPIED column — the arcade rule.
		const Ball* shooters[kEnemyCols] = {};
		for (uint32_t i = 0; i < m_enemies.size(); ++i) {
			const Ball& e = m_enemies[i];
			if (!e.alive)
				continue;
			const uint32_t col = i % kEnemyCols;
			if (!shooters[col] || e.pos.x > shooters[col]->pos.x)
				shooters[col] = &e;
		}
		uint32_t candidates = 0;
		for (const Ball* s : shooters)
			candidates += s ? 1u : 0u;
		if (!candidates)
			return;
		uint32_t pick = Rng() % candidates;
		const Ball* src = nullptr;
		for (const Ball* s : shooters)
			if (s && pick-- == 0) {
				src = s;
				break;
			}
		if (!src)
			return;
		for (Ball& b : m_enemyBullets) {
			if (b.alive)
				continue;
			b.alive = true;
			SetBallPos(b, src->pos
				+ glm::vec3(src->radius + kBulletRadius + 0.05f, 0.0f, 0.0f));
			ShowBall(b);
			return;
		}
	}

	void InvadersElement::KillPlayer() {
		// Green shrapnel pop (bigger than an enemy pop — it hurts).
		m_renderer.SpawnParticleBurst(m_player.pos,
			glm::vec3(0.30f, 1.00f, 0.35f), 3.6f, 0.035f, 1.0f, 140);
		HideBall(m_player);
		m_player.alive = false;
		--m_lives;
		if (m_lives > 0) {
			m_phase = Phase::PlayerDead;
			m_phaseTimer = 1.3f;
		}
		else {
			m_phase = Phase::GameOver;
			m_phaseTimer = 0.0f;
			std::cerr << "[Invaders] game over: score " << m_score << "\n";
		}
	}

	void InvadersElement::onUpdate(float dt) {
		if (!m_attached || !m_world)
			return;
		Scene& scene = m_world->scene;
		// The world was rebuilt under us (a re-resolve shrank the node
		// table: our mirror nodes are gone). Switch off instead of
		// indexing out of bounds — the viewer keeps running.
		if (m_player.node == Ball::kNoNode
			|| m_player.node >= scene.hierarchy.size()) {
			m_attached = false;
			std::cerr << "[Invaders] world rebuilt under the game - invaders off\n";
			return;
		}
		m_time += dt;

		// ENTER / BACKSPACE: crude reset from any phase.
		if (m_input.isKeyPressed(SDL_SCANCODE_RETURN)
			|| m_input.isKeyPressed(SDL_SCANCODE_KP_ENTER)
			|| m_input.isKeyPressed(SDL_SCANCODE_BACKSPACE)) {
			ResetGame();
			return;
		}

		m_invulnTimer = std::max(0.0f, m_invulnTimer - dt);
		m_fireCooldown = std::max(0.0f, m_fireCooldown - dt);
		m_enemyFireCooldown = std::max(0.0f, m_enemyFireCooldown - dt);

		// ---- phase timers ----
		if (m_phase == Phase::PlayerDead) {
			m_phaseTimer -= dt;
			if (m_phaseTimer <= 0.0f) {
				m_phase = Phase::Playing;
				m_player.alive = true;
				m_invulnTimer = 2.0f;
				SetBallPos(m_player, glm::vec3(kPlayerX, kPlayerRadius, kEnemyCenterZ));
			}
		}
		else if (m_phase == Phase::WaveClear) {
			m_phaseTimer -= dt;
			if (m_phaseTimer <= 0.0f) {
				++m_wave;
				m_phase = Phase::Playing;
				StartWave();
				std::cerr << "[Invaders] wave " << m_wave << " incoming\n";
			}
		}

		// ---- player ----
		if (m_phase == Phase::Playing && m_player.alive) {
			// Strafe across the corridor. Screen-right = world -Z (the
			// rotated basis), so D (right) decreases z and A increases it.
			float mz = 0.0f;
			if (m_input.isKeyHeld(SDL_SCANCODE_A)
				|| m_input.isKeyHeld(SDL_SCANCODE_LEFT))
				mz += 1.0f;
			if (m_input.isKeyHeld(SDL_SCANCODE_D)
				|| m_input.isKeyHeld(SDL_SCANCODE_RIGHT))
				mz -= 1.0f;
			glm::vec3 p = m_player.pos;
			p.x = kPlayerX;
			p.y = kPlayerRadius;
			p.z = glm::clamp(p.z + mz * kPlayerSpeed * dt,
				kPlayerZMin, kPlayerZMax);
			SetBallPos(m_player, p);
			// Shields-up blink while invulnerable.
			if (m_invulnTimer > 0.0f) {
				if (std::fmod(m_invulnTimer, 0.3f) < 0.18f)
					ShowBall(m_player);
				else
					HideBall(m_player);
			}
			else {
				ShowBall(m_player);
			}
			if (m_fireCooldown <= 0.0f && m_input.isKeyHeld(SDL_SCANCODE_SPACE)) {
				FirePlayerBullet();
				m_fireCooldown = kPlayerFireCooldown;
			}
		}

		// ---- invader formation march (pauses while the player is down,
		// like the arcade original) ----
		uint32_t aliveCount = 0;
		for (const Ball& e : m_enemies)
			aliveCount += e.alive ? 1u : 0u;
		const float aliveFrac = float(aliveCount) / float(m_enemies.size());

		if (m_phase == Phase::Playing && aliveCount > 0) {
			// Speed scales with the wave AND with how few remain (the
			// classic end-game scurry) — deliberately CALM at full strength:
			// the advance is a small step per bounce and the strafe swings
			// are WIDE (0.44 m per half-swing at 0.6 m/s = one bounce every
			// ~0.73 s), so the front row needs ~25-30 s to walk the nave at
			// full strength (the first cuts bounced every ~0.33 s and landed
			// the grid on the player in under 10 s — unplayable).
			const float march = 0.6f * (1.0f + 0.12f * float(m_wave - 1))
				* (1.0f + 1.3f * (1.0f - aliveFrac));
			const float prevZ = m_formation.x;
			m_formation.x += m_enemyDir * march * dt;
			bool bounced = false;
			if (m_formation.x > kFormationZLimit) {
				m_formation.x = kFormationZLimit;
				m_enemyDir = -1.0f;
				bounced = true;
			}
			else if (m_formation.x < -kFormationZLimit) {
				m_formation.x = -kFormationZLimit;
				m_enemyDir = 1.0f;
				bounced = true;
			}
			const float dz = m_formation.x - prevZ; // post-clamp delta
			bool breached = false;
			uint32_t slot = 0;
			for (Ball& e : m_enemies) {
				if (!e.alive)
					continue;
				e.pos.z += dz;
				if (bounced)
					e.pos.x += kFormationAdvance;
				if (e.pos.x > kBreachX)
					breached = true;
				// Idle bob (y is always recomputed from the radius — it
				// never accumulates the formation offsets).
				const float bob = 0.07f * std::sin(m_time * 3.0f + float(slot) * 0.7f);
				SetBallPos(e, glm::vec3(e.pos.x, kEnemyRadius + bob, e.pos.z));
				ShowBall(e);
				++slot;
			}
			if (breached) {
				// Invaders landed: straight to game over.
				m_lives = 0;
				KillPlayer();
			}
			else if (m_enemyFireCooldown <= 0.0f) {
				FireEnemyBullet();
				const float base = 1.9f - 0.15f * float(m_wave - 1);
				m_enemyFireCooldown =
					std::max(0.35f, base * (0.35f + 0.65f * aliveFrac));
			}
		}

		// ---- player bullets (straight -X, up the nave) ----
		for (Ball& b : m_playerBullets) {
			if (!b.alive)
				continue;
			const glm::vec3 p = b.pos + glm::vec3(-kPlayerBulletSpeed * dt, 0.0f, 0.0f);
			if (p.x < kBulletTopX) {
				b.alive = false;
				HideBall(b);
				// Fizzle where the bullet dies at the field's top edge.
				m_renderer.SpawnParticleBurst(p,
					glm::vec3(1.0f, 0.95f, 0.60f), 0.8f, 0.012f, 0.30f, 8);
				continue;
			}
			Ball* hit = nullptr;
			for (Ball& e : m_enemies) {
				if (e.alive && glm::distance(p, e.pos) < e.radius + b.radius) {
					hit = &e;
					break;
				}
			}
			if (hit) {
				// Red shrapnel pop at the kill point.
				m_renderer.SpawnParticleBurst(hit->pos,
					glm::vec3(1.0f, 0.30f, 0.08f), 3.2f, 0.030f, 0.8f, 90);
				hit->alive = false;
				HideBall(*hit);
				b.alive = false;
				HideBall(b);
				m_score += 100;
				continue;
			}
			SetBallPos(b, p);
		}

		// ---- invader bullets (straight +X, down the nave) ----
		const float enemyBulletSpeed =
			kEnemyBulletSpeedBase + kEnemyBulletSpeedPerWave * float(m_wave - 1);
		for (Ball& b : m_enemyBullets) {
			if (!b.alive)
				continue;
			const glm::vec3 p = b.pos + glm::vec3(enemyBulletSpeed * dt, 0.0f, 0.0f);
			if (p.x > kBulletBottomX) {
				b.alive = false;
				HideBall(b);
				// Fizzle where the shot dies at the field's bottom edge.
				m_renderer.SpawnParticleBurst(p,
					glm::vec3(1.0f, 0.35f, 0.12f), 0.8f, 0.012f, 0.30f, 8);
				continue;
			}
			if (m_phase == Phase::Playing && m_player.alive
				&& m_invulnTimer <= 0.0f
				&& glm::distance(p, m_player.pos) < m_player.radius + b.radius) {
				b.alive = false;
				HideBall(b);
				KillPlayer();
				continue;
			}
			SetBallPos(b, p);
		}

		// ---- wave cleared? ----
		aliveCount = 0;
		for (const Ball& e : m_enemies)
			aliveCount += e.alive ? 1u : 0u;
		if (m_phase == Phase::Playing && aliveCount == 0) {
			m_phase = Phase::WaveClear;
			m_phaseTimer = 2.0f;
			m_score += 500;
			for (Ball& b : m_enemyBullets) {
				b.alive = false;
				HideBall(b);
			}
			// Golden celebration pop above the player.
			m_renderer.SpawnParticleBurst(
				m_player.pos + glm::vec3(0.0f, 1.2f, 0.0f),
				glm::vec3(1.0f, 0.85f, 0.25f), 4.5f, 0.040f, 1.4f, 160);
			std::cerr << "[Invaders] wave " << m_wave << " cleared (+500)\n";
		}

		// Heartbeat (~2 s at 60 Hz): one stderr line for live debugging —
		// phase/score/enemies plus the player's link state, so a capture
		// run can be read back from the log alone.
		if (++m_logFrame % 120u == 0u) {
			const char* phaseName = "?";
			switch (m_phase) {
			case Phase::Playing: phaseName = "playing"; break;
			case Phase::PlayerDead: phaseName = "playerdead"; break;
			case Phase::WaveClear: phaseName = "waveclear"; break;
			case Phase::GameOver: phaseName = "gameover"; break;
			}
			bool playerInList = false;
			for (const auto& item : m_world->drawItems)
				if (item.node == m_player.node) {
					playerInList = true;
					break;
				}
			std::cerr << "[Invaders] t=" << m_time << "s " << phaseName
				<< " score=" << m_score << " lives=" << m_lives
				<< " enemies=" << aliveCount
				<< " player(" << (m_player.alive ? "alive" : "down")
				<< (m_world->scene.meshForNode.contains(m_player.node)
					? "+linked" : "+unlinked")
				<< (playerInList ? "+listed" : "+unlisted")
				<< " x=" << m_player.pos.x << " z=" << m_player.pos.z
				<< ") form=(" << m_formation.x << "," << m_formation.y
				<< ") meshLinks=" << m_world->scene.meshForNode.size()
				<< " drawItems=" << m_world->drawItems.size() << "\n";
		}

		// Fold every SetBallPos mark into the merged-scene globals — the
		// renderer's draw record reads globalTransform fresh this frame.
		recalculateGlobalTransforms(scene);
	}

	void InvadersElement::DrawHud() {
		if (!m_attached)
			return;
		const ImVec2 disp = ImGui::GetIO().DisplaySize;
		ImDrawList* dl = ImGui::GetForegroundDrawList();
		// Shadowed text (the DS-viz label recipe): a black offset underlay
		// under every line so the HUD reads on any background.
		auto text = [dl](const char* s, float y, ImU32 col, float x, bool centerX) {
			const ImVec2 ts = ImGui::CalcTextSize(s);
			const float px = centerX ? x - ts.x * 0.5f : x;
			dl->AddText(ImVec2(px + 2.0f, y + 2.0f), IM_COL32(0, 0, 0, 190), s);
			dl->AddText(ImVec2(px, y), col, s);
		};

		// Score strip, top-center.
		char line[96];
		std::snprintf(line, sizeof(line), "SCORE %d    WAVE %u    LIVES %d",
			m_score, m_wave, m_lives);
		text(line, 12.0f, IM_COL32(235, 235, 235, 255), disp.x * 0.5f, true);

		// Center message by phase.
		const char* msg = nullptr;
		ImU32 msgCol = IM_COL32(255, 255, 255, 255);
		if (m_phase == Phase::GameOver) {
			msg = "GAME OVER  --  ENTER TO RESTART";
			msgCol = IM_COL32(255, 96, 72, 255);
		}
		else if (m_phase == Phase::WaveClear) {
			msg = "WAVE CLEARED";
			msgCol = IM_COL32(128, 255, 138, 255);
		}
		else if (m_phase == Phase::PlayerDead) {
			msg = "HIT!";
			msgCol = IM_COL32(255, 96, 72, 255);
		}
		else if (m_invulnTimer > 0.0f) {
			msg = "SHIELDS UP";
			msgCol = IM_COL32(128, 255, 138, 255);
		}
		if (msg)
			text(msg, disp.y * 0.32f, msgCol, disp.x * 0.5f, true);

		// Controls hint, bottom-left.
		text("A/D or ARROWS: move    SPACE: fire    ENTER: restart",
			disp.y - 30.0f, IM_COL32(200, 200, 200, 220), 12.0f, false);
	}

} // namespace app
