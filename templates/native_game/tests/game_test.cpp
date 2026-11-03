// SPDX-License-Identifier: MIT
#include "game.h"
#include <cassert>
#include <cstdio>
int main() {
    starter::State game;
    const double initial = game.favor;
    game.tick(0.5); // capped delta; no simulation explosion on stalls
    assert(game.favor > initial);
    assert(game.apply(starter::Action::Offer));
    assert(game.offerings == 1 && game.score == 10);
    const double income = game.productionPerSecond();
    assert(game.apply(starter::Action::Upgrade));
    assert(game.upgrades == 1 && game.productionPerSecond() > income);
    const auto old = game.favor;
    assert(!game.apply(starter::Action::Defend)); // insufficient funds
    assert(game.favor == old);
    assert(game.apply(starter::Action::Offer));
    assert(game.apply(starter::Action::Offer));
    assert(game.apply(starter::Action::Offer));
    assert(game.apply(starter::Action::Defend));
    assert(game.defenses == 1);
    game.tick(-1);
    assert(game.favor >= 0);
    std::puts("native game state tests passed");
}
