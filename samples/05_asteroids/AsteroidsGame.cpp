// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "AsteroidsGame.h"
#include <algorithm>
#include <cmath>

namespace asteroids {
namespace {
constexpr float Pi = 3.14159265358979323846f;
Vec2 operator+(Vec2 a, Vec2 b) {
    return {a.x + b.x, a.y + b.y};
}
Vec2 operator*(Vec2 a, float k) {
    return {a.x * k, a.y * k};
}
Vec2 Facing(float a) {
    return {std::cos(a), std::sin(a)};
}

void Wrap(Vec2& p) {
    if (p.x < 0.0f)
        p.x += Width;
    if (p.x >= Width)
        p.x -= Width;
    if (p.y < 0.0f)
        p.y += Height;
    if (p.y >= Height)
        p.y -= Height;
}
// Across-edge collisions behave exactly like on-screen collisions.
float WrappedDistanceSquared(Vec2 a, Vec2 b) {
    float dx = std::abs(a.x - b.x);
    float dy = std::abs(a.y - b.y);
    dx = std::min(dx, Width - dx);
    dy = std::min(dy, Height - dy);
    return dx * dx + dy * dy;
}
float Radius(int tier) {
    return tier == 3 ? 5.0f : tier == 2 ? 3.1f : 1.7f;
}
} // namespace

uint32_t Game::NextRandom() {
    return m_rng.NextU32();
}
float Game::Random(float lo, float hi) {
    return lo + (hi - lo) * m_rng.NextFloat01();
}
void Game::Reset() {
    m_rng.Restore(m_initialRandomState);
    m_state = State::Playing;
    m_ship = Ship{};
    m_ship.invulnerable = 2.5f;
    m_score = 0;
    m_lives = 3;
    m_wave = 1;
    m_seconds = 0.0f;
    m_fireCooldown = 0.0f;
    m_waveDelay = 0.0f;
    m_rocks.clear();
    m_bullets.clear();
    m_sparks.clear();
    SpawnWave();
}
void Game::SpawnWave() {
    const int count = std::min(3 + m_wave, 10);
    for (int i = 0; i < count; ++i) {
        Asteroid rock{};
        // Never spawn a new asteroid right on top of the center ship.
        do {
            rock.position = {Random(0.0f, Width), Random(0.0f, Height)};
        } while (WrappedDistanceSquared(rock.position, {Width * 0.5f, Height * 0.5f}) <
                 18.0f * 18.0f);
        const float heading = Random(0.0f, 2.0f * Pi);
        rock.velocity = Facing(heading) * Random(6.0f, 11.0f + m_wave);
        rock.angle = Random(0.0f, 2.0f * Pi);
        rock.spin = Random(-1.1f, 1.1f);
        rock.shape = NextRandom();
        m_rocks.push_back(rock);
    }
    m_ship.invulnerable = std::max(m_ship.invulnerable, 1.5f);
}
void Game::Burst(Vec2 position, int count) {
    for (int i = 0; i < count; ++i) {
        const float a = Random(0.0f, 2.0f * Pi);
        const float speed = Random(5.0f, 23.0f);
        const float life = Random(0.25f, 0.75f);
        m_sparks.push_back({position, Facing(a) * speed, life, life});
    }
}
void Game::BreakRock(size_t index) {
    const Asteroid rock = m_rocks[index];
    m_rocks.erase(m_rocks.begin() + static_cast<std::ptrdiff_t>(index));
    m_score += rock.tier == 3 ? 20 : rock.tier == 2 ? 50 : 100;
    Burst(rock.position, rock.tier == 3 ? 12 : 8);
    if (rock.tier > 1) {
        const float base = std::atan2(rock.velocity.y, rock.velocity.x);
        for (int sign : {-1, 1}) {
            Asteroid child{};
            child.position = rock.position;
            child.tier = rock.tier - 1;
            child.angle = Random(0.0f, 2.0f * Pi);
            child.spin = Random(-2.0f, 2.0f);
            child.velocity = Facing(base + float(sign) * 0.85f) * Random(13.0f, 18.0f + m_wave);
            child.shape = NextRandom();
            m_rocks.push_back(child);
        }
    }
}
void Game::Step(const Input& input, float dt) {
    if (m_state != State::Playing) {
        if (input.start)
            Reset();
        return;
    }
    // Host uses 120 Hz fixed ticks. Defend against bad external deltas.
    dt = std::clamp(dt, 0.0f, 1.0f / 30.0f);
    if (dt == 0.0f)
        return;
    m_seconds += dt;
    m_fireCooldown = std::max(0.0f, m_fireCooldown - dt);
    m_ship.invulnerable = std::max(0.0f, m_ship.invulnerable - dt);

    m_ship.angle += (float(input.right) - float(input.left)) * 3.5f * dt;
    if (input.thrust)
        m_ship.velocity = m_ship.velocity + Facing(m_ship.angle) * (43.0f * dt);
    m_ship.velocity = m_ship.velocity * std::exp(-0.35f * dt);
    m_ship.position = m_ship.position + m_ship.velocity * dt;
    Wrap(m_ship.position);

    if (input.fire && m_fireCooldown <= 0.0f && m_bullets.size() < 8) {
        const Vec2 facing = Facing(m_ship.angle);
        m_bullets.push_back(
            {m_ship.position + facing * 2.2f, m_ship.velocity + facing * 64.0f, 1.0f});
        m_fireCooldown = 0.16f;
    }
    for (Asteroid& rock : m_rocks) {
        rock.position = rock.position + rock.velocity * dt;
        rock.angle += rock.spin * dt;
        Wrap(rock.position);
    }
    for (size_t i = 0; i < m_bullets.size();) {
        Bullet& bullet = m_bullets[i];
        bullet.position = bullet.position + bullet.velocity * dt;
        bullet.lifetime -= dt;
        Wrap(bullet.position);
        bool hit = false;
        if (bullet.lifetime > 0.0f) {
            for (size_t j = 0; j < m_rocks.size(); ++j) {
                const float r = Radius(m_rocks[j].tier) + 0.4f;
                if (WrappedDistanceSquared(bullet.position, m_rocks[j].position) < r * r) {
                    BreakRock(j);
                    hit = true;
                    break;
                }
            }
        }
        if (hit || bullet.lifetime <= 0.0f)
            m_bullets.erase(m_bullets.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
    if (m_ship.invulnerable <= 0.0f) {
        for (const Asteroid& rock : m_rocks) {
            const float r = Radius(rock.tier) + 1.25f;
            if (WrappedDistanceSquared(m_ship.position, rock.position) >= r * r)
                continue;
            Burst(m_ship.position, 24);
            --m_lives;
            m_ship = Ship{};
            m_ship.invulnerable = 2.5f;
            m_bullets.clear();
            if (m_lives == 0)
                m_state = State::GameOver;
            break;
        }
    }
    for (size_t i = 0; i < m_sparks.size();) {
        Spark& spark = m_sparks[i];
        spark.position = spark.position + spark.velocity * dt;
        spark.lifetime -= dt;
        Wrap(spark.position);
        if (spark.lifetime <= 0.0f)
            m_sparks.erase(m_sparks.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
    if (m_rocks.empty() && m_state == State::Playing) {
        m_waveDelay += dt;
        if (m_waveDelay >= 1.5f) {
            ++m_wave;
            m_waveDelay = 0.0f;
            SpawnWave();
        }
    } else
        m_waveDelay = 0.0f;
}
} // namespace asteroids
