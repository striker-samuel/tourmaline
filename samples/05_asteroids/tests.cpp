// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "AsteroidsGame.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::cerr << "Asteroids check failed: " << #condition << " at " << __FILE__ << ":"     \
                      << __LINE__ << "\\n";                                                        \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

int main() {
    asteroids::Game game;
    CHECK(game.state() == asteroids::Game::State::Ready);
    CHECK(game.rocks().empty());
    game.Step({.start = true}, 1.0f / 120.0f);
    CHECK(game.state() == asteroids::Game::State::Playing);
    CHECK(game.wave() == 1 && game.lives() == 3 && game.score() == 0);
    CHECK(game.rocks().size() == 4);
    const auto before = game.ship().position;
    for (int i = 0; i < 24; ++i)
        game.Step({.thrust = true, .fire = true}, 1.0f / 120.0f);
    CHECK(game.ship().position.y < before.y);
    CHECK(!game.bullets().empty() && game.bullets().size() <= 8);
    const auto angle = game.ship().angle;
    game.Step({.right = true}, 1.0f / 120.0f);
    CHECK(game.ship().angle > angle);
    const auto firstShape = game.rocks().front().shape;
    game.Reset();
    CHECK(game.score() == 0 && game.lives() == 3 && game.wave() == 1);
    CHECK(game.bullets().empty() && game.sparks().empty());
    CHECK(game.rocks().size() == 4);
    CHECK(game.rocks().front().shape == firstShape);
    CHECK(std::abs(game.ship().position.x - asteroids::Width * 0.5f) < 0.001f);
    // New engine PCG32 service: seeded runs are repeatable, while a
    // different master seed changes the first wave.
    tour::RandomService service(42);
    asteroids::Game seeded, replay, different;
    seeded.SetRandomStream(service.MakeStream("asteroids"));
    replay.SetRandomStream(tour::RandomService(42).MakeStream("asteroids"));
    different.SetRandomStream(tour::RandomService(99).MakeStream("asteroids"));
    seeded.Reset();
    replay.Reset();
    different.Reset();
    CHECK(seeded.rocks().size() == replay.rocks().size());
    for (size_t i = 0; i < seeded.rocks().size(); ++i) {
        CHECK(seeded.rocks()[i].shape == replay.rocks()[i].shape);
        CHECK(seeded.rocks()[i].position.x == replay.rocks()[i].position.x);
        CHECK(seeded.rocks()[i].position.y == replay.rocks()[i].position.y);
    }
    CHECK(seeded.rocks()[0].shape != different.rocks()[0].shape);
    const auto originalShape = seeded.rocks()[0].shape;
    seeded.Step({.thrust = true, .fire = true}, 1.0f / 120.0f);
    seeded.Reset(); // Restarts reuse the original stream checkpoint.
    CHECK(seeded.rocks()[0].shape == originalShape);

    std::puts("asteroids_sim_tests: ok");
}
