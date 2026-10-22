// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Crude SPACE INVADERS inside the sponza scene — the ASTEROIDS.exe game
// host's game layer (asteroids/src/main.cpp installs this element through
// RunPakApp's extra-element hook; the plain App.exe viewer never stacks
// it).
//
// The sponza check scene grows a mini arcade layer on top of the loaded
// world: a fixed TOP-DOWN camera over the atrium floor, a GREEN ball
// player (A/D + arrows to move, SPACE to fire), a grid of RED ball
// invaders that shuffles side to side and advances toward the player,
// emissive ball bullets both ways, lives/score/waves. Everything renders
// through the normal World path: the balls are procedural UV spheres
// (the chrome-ball recipe from PakApp.cpp) uploaded to the world's
// DYNAMIC cluster, linked into the merged scene as root nodes, and
// driven per frame by rewriting localTransform + the
// markAsChanged/recalculateGlobalTransforms pair (the editor's
// live-surgery path — no resolve needed; see editor_commands.cpp).
// Show/hide is drawItems surgery (the terrain create/uncreate
// precedent): a hidden ball carries no meshForNode link and no
// drawItems entry, so no pass sees it.
//
// This is an IAppElement stacked AFTER the WorldRendererElement: its
// onUpdate runs the game logic once the renderer element's control pass
// is done, and it records no passes itself (the World renderer draws
// the balls; the HUD rides the renderer's ImGui pass through
// WorldRenderer::SetExtraUi). The camera: the host disables the
// renderer element's FPS camera control and this element frames the
// positioner straight down through the shared ResolveLookAtUp
// convention — screen-up = world -Z, so the player row at +Z sits at
// the BOTTOM of the screen and the invaders march "down" toward it,
// like the arcade original.

#include <Engine/App/Element.h>
#include <Engine/App/app.h>
#include <Engine/App/world.h>
#include <Engine/Renderer/WorldRenderer.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace app {

class InvadersElement final : public engine::IAppElement {
public:
	// worldHandle: filled by the host's world-source lambda as it hands
	// the loaded world to the renderer (the renderer owns the LinearPtr;
	// the raw pointer stays valid for this element's lifetime — detach
	// order is reverse-stack, so the game detaches before the renderer
	// tears the world down).
	InvadersElement(smvk::App& host, engine::WorldRenderer& renderer,
		std::shared_ptr<smvk::World*> worldHandle);

	const char* ElementName() const override { return "Invaders"; }

	// IAppElement lifecycle. onAttach builds the balls into the loaded
	// world + frames the camera (the renderer element has attached —
	// and thus loaded the world — before this element, by stack order);
	// onRender records nothing (the World renderer draws the balls).
	bool onAttach() override;
	void onDetach() override;
	void onUpdate(float dt) override;
	void onRender(engine::ElementFrame& frame) override { (void)frame; }

private:
	// One ball entity: merged-scene root node + the world pool rows the
	// draw list links to. "alive" is the GAME state; visibility follows
	// it through Show/Hide (ShowBall early-outs when already linked, so
	// calling it every frame on live balls is cheap).
	struct Ball {
		static constexpr uint32_t kNoNode = UINT32_MAX;
		uint32_t node = kNoNode;        // merged-scene node id
		uint32_t meshIdx = UINT32_MAX;  // world meshLocations row
		uint32_t matIdx = UINT32_MAX;   // world materialLocations row
		float radius = 0.3f;
		bool alive = false;
		glm::vec3 pos{ 0.0f }; // current WORLD position (SetBallPos keeps it in sync)
	};

	enum class Phase { Playing, PlayerDead, WaveClear, GameOver };

	// ---- entity plumbing (editor live-surgery precedent) ----
	bool CreateBall(Ball& b, float radius, int slices, int stacks,
		uint32_t matIdx, const char* name);
	void ShowBall(Ball& b);
	void HideBall(Ball& b);
	// Writes the node's local transform + marks it changed; the frame's
	// tail recalculateGlobalTransforms() folds the marks into globals.
	void SetBallPos(Ball& b, const glm::vec3& p);

	// ---- game flow ----
	void ResetGame();
	void StartWave();
	void FirePlayerBullet();
	void FireEnemyBullet();
	void KillPlayer();
	void DrawHud(); // SetExtraUi hook: runs inside the renderer's ImGui build
	uint32_t Rng() { m_rng = m_rng * 1664525u + 1013904223u; return m_rng >> 8; }

	smvk::App& m_host;
	Input& m_input;
	engine::WorldRenderer& m_renderer;
	std::shared_ptr<smvk::World*> m_worldHandle;
	smvk::World* m_world = nullptr;
	bool m_attached = false;

	// ---- game state ----
	Phase m_phase = Phase::Playing;
	uint32_t m_wave = 1;
	int m_lives = 3;
	int m_score = 0;
	float m_phaseTimer = 0.0f;  // PlayerDead / WaveClear countdown
	float m_invulnTimer = 0.0f; // post-respawn shields (blink + no-hit)
	float m_time = 0.0f;
	uint32_t m_rng = 1;
	uint32_t m_logFrame = 0; // heartbeat counter (the ~2 s stderr line)

	Ball m_player{};
	static constexpr uint32_t kEnemyRows = 3;
	static constexpr uint32_t kEnemyCols = 5; // across the corridor width (see InvadersGame.cpp)
	std::array<Ball, kEnemyRows * kEnemyCols> m_enemies{};
	// Formation march state: .x = the strafe offset along Z (the corridor
	// width); the per-bounce +X advance lands on the balls' positions
	// directly (never accumulated here).
	glm::vec2 m_formation{ 0.0f, 0.0f };
	float m_enemyDir = 1.0f;

	static constexpr uint32_t kMaxPlayerBullets = 4;
	static constexpr uint32_t kMaxEnemyBullets = 8;
	std::array<Ball, kMaxPlayerBullets> m_playerBullets{};
	std::array<Ball, kMaxEnemyBullets> m_enemyBullets{};
	float m_fireCooldown = 0.0f;      // player bullet rate cap
	float m_enemyFireCooldown = 0.0f; // invader shot scheduling

	// World material rows (built once at attach from the dynamic cluster).
	uint32_t m_matPlayer = UINT32_MAX;
	uint32_t m_matEnemy = UINT32_MAX;
	uint32_t m_matBulletPlayer = UINT32_MAX;
	uint32_t m_matBulletEnemy = UINT32_MAX;
};

} // namespace app
