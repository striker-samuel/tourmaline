// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <TourmalineEditorEntry.h>
#include <cstdio>
#include <cstdlib>
#include <memory>

// CHECK must evaluate its operand in optimized builds (unlike assert).
#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "CHECK failed: %s at line %d\\n", #expr, __LINE__); \
        std::abort(); \
    } \
} while (false)

int main() {
    editor::GameSessionRunner runner;
    int updates = 0, stops = 0;
    float total = 0.0f;
    runner.Stop();
    runner.Tick(1.0f);
    CHECK(!runner.Active());
    auto state = std::make_shared<int>(42);
    CHECK(runner.Start({
        .onUpdate = [&, state](float dt) {
            ++updates;
            total += dt;
            CHECK(*state == 42);
        },
        .onStop = [&, state] { ++stops; CHECK(*state == 42); }
    }));
    CHECK(!runner.Start({})); // cannot replace a running game
    runner.Tick(0.25f);
    runner.Tick(0.75f);
    CHECK(updates == 2 && total == 1.0f);
    runner.Stop();
    runner.Stop();
    runner.Tick(1.0f);
    CHECK(stops == 1 && updates == 2 && !runner.Active());
    CHECK(runner.Start({})); // entry-only game without callbacks
    runner.Stop();
    return 0;
}
