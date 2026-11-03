// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <Engine/Core/Random.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace asteroids {

// The simulation uses a fixed, resolution-independent logical playfield.
inline constexpr float Width = 100.0f;
inline constexpr float Height = 56.25f;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};
struct Input {
    bool left = false;
    bool right = false;
    bool thrust = false;
    bool fire = false;
    bool start = false;
};
struct Ship {
    Vec2 position{ Width * 0.5f, Height * 0.5f };
    Vec2 velocity{};
    float angle = -1.5707963268f;
    float invulnerable = 0.0f;
};
struct Asteroid {
    Vec2 position;
    Vec2 velocity;
    float angle = 0.0f;
    float spin = 0.0f;
    int tier = 3;
    uint32_t shape = 0;
};
struct Bullet {
    Vec2 position;
    Vec2 velocity;
    float lifetime = 0.0f;
};
struct Spark {
    Vec2 position;
    Vec2 velocity;
    float lifetime = 0.0f;
    float maxLifetime = 0.0f;
};
class Game {
public:
    enum class State { Ready, Playing, GameOver };
    // Ready/GameOver: Input::start starts or restarts the game.
    void Step(const Input& input, float dt);
    void Reset();
    // One independent stream per game, derived by Context::random().
    // Restarts restore the same first-wave sequence for a fixed seed.
    void SetRandomStream(tour::RandomStream stream) {
        m_rng = stream;
        m_initialRandomState = stream.Save();
    }

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] int score() const { return m_score; }
    [[nodiscard]] int lives() const { return m_lives; }
    [[nodiscard]] int wave() const { return m_wave; }
    [[nodiscard]] float seconds() const { return m_seconds; }
    [[nodiscard]] float waveDelay() const { return m_waveDelay; }
    [[nodiscard]] const Ship& ship() const { return m_ship; }
    [[nodiscard]] const std::vector<Asteroid>& rocks() const { return m_rocks; }
    [[nodiscard]] const std::vector<Bullet>& bullets() const { return m_bullets; }
    [[nodiscard]] const std::vector<Spark>& sparks() const { return m_sparks; }
private:
    void SpawnWave();
    void BreakRock(size_t index);
    void Burst(Vec2 position, int count);
    uint32_t NextRandom();
    float Random(float lo, float hi);

    State m_state = State::Ready;
    Ship m_ship{};
    std::vector<Asteroid> m_rocks;
    std::vector<Bullet> m_bullets;
    std::vector<Spark> m_sparks;
    tour::RandomStream m_rng{0x41535452u};
    tour::RandomStream::State m_initialRandomState = m_rng.Save();
    int m_score = 0;
    int m_lives = 3;
    int m_wave = 1;
    float m_seconds = 0.0f;
    float m_fireCooldown = 0.0f;
    float m_waveDelay = 0.0f;
};
} // namespace asteroids
