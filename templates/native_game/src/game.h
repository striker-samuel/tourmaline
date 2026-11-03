// SPDX-License-Identifier: MIT
// Game-owned state. No graphics API, SDL or engine type in this file.
#pragma once
#include <algorithm>
#include <cmath>

namespace starter {
enum class Action { Offer, Upgrade, Defend };

struct State {
    double favor = 3.0;
    int offerings = 0;
    int upgrades = 0;
    int defenses = 0;
    int score = 0;

    [[nodiscard]] double productionPerSecond() const {
        return 1.0 + 0.5 * upgrades;
    }
    [[nodiscard]] double upgradeCost() const {
        return 3.0 + 2.0 * upgrades;
    }

    void tick(double dt) {
        if (!std::isfinite(dt) || dt <= 0) return;
        favor += std::min(dt, 0.1) * productionPerSecond();
    }
    [[nodiscard]] bool apply(Action action) {
        switch (action) {
        case Action::Offer:
            ++offerings;
            favor += 2.0;
            score += 10;
            return true;
        case Action::Upgrade:
            if (favor < upgradeCost()) return false;
            favor -= upgradeCost();
            ++upgrades;
            score += 40;
            return true;
        case Action::Defend:
            if (favor < 5.0) return false;
            favor -= 5.0;
            ++defenses;
            score += 60;
            return true;
        }
        return false;
    }
};
} // namespace starter
