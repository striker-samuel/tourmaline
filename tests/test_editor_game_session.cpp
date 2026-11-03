// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <TourmalineEditorEntry.h>
#include <cassert>
#include <memory>

int main() {
    editor::GameSessionRunner runner;
    int updates = 0, stops = 0;
    float total = 0.0f;
    runner.Stop();
    runner.Tick(1.0f);
    assert(!runner.Active());
    auto state = std::make_shared<int>(42);
    assert(runner.Start({
        .onUpdate = [&, state](float dt) {
            ++updates;
            total += dt;
            assert(*state == 42);
        },
        .onStop = [&, state] { ++stops; assert(*state == 42); }
    }));
    assert(!runner.Start({})); // cannot replace a running game
    runner.Tick(0.25f);
    runner.Tick(0.75f);
    assert(updates == 2 && total == 1.0f);
    runner.Stop();
    runner.Stop();
    runner.Tick(1.0f);
    assert(stops == 1 && updates == 2 && !runner.Active());
    assert(runner.Start({})); // entry-only game without callbacks
    runner.Stop();
    return 0;
}
