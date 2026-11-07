// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Scene/scene.h>
#include <Engine/Scene/trigger_volume.h>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

using namespace tour::scene;

int main() {
    int failed = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++failed;
    };
    Scene scene;
    const int node = addNode(scene, -1, 0);
    scene.globalTransform[size_t(node)] = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)) *
                                          glm::scale(glm::mat4(1.0f), glm::vec3(8, 4, 8));
    SceneTriggerVolume trigger{};
    trigger.tag = "Door";
    check(SetTriggerVolume(scene, uint32_t(node), trigger), "attach trigger");
    check(SetGiVolume(scene, uint32_t(node), SceneGiVolume{}) &&
              SetAudioRoom(scene, uint32_t(node), SceneAudioRoom{}),
          "GI, audio and trigger coexist on same volume");
    TriggerVolumeTracker tracker;
    std::vector<TriggerEvent> events;
    auto collect = [&](const TriggerEvent& event) { events.push_back(event); };
    tracker.Update(scene, 1, glm::vec3(0, 0, 0), collect);
    check(events.empty(), "outside does not enter");
    tracker.Update(scene, 1, glm::vec3(11, 0, 0), collect);
    check(events.size() == 1 && events[0].type == TriggerEvent::Type::Enter &&
              events[0].tag == "Door",
          "enter event carries tag");
    tracker.Update(scene, 1, glm::vec3(11, 0, 0), collect);
    check(events.size() == 1, "staying inside does not repeat");
    tracker.Update(scene, 1, glm::vec3(20, 0, 0), collect);
    check(events.size() == 2 && events[1].type == TriggerEvent::Type::Exit, "exit on leaving");
    auto* row = FindTriggerVolume(scene, uint32_t(node));
    row->trigger.once = true;
    tracker.Update(scene, 2, glm::vec3(11, 0, 0), collect);
    tracker.Update(scene, 2, glm::vec3(20, 0, 0), collect);
    tracker.Update(scene, 2, glm::vec3(11, 0, 0), collect);
    check(events.size() == 4, "once suppresses second visit per actor");
    check(RemoveTriggerVolume(scene, uint32_t(node)) &&
              FindTriggerVolume(scene, uint32_t(node)) == nullptr &&
              FindGiVolume(scene, uint32_t(node)) != nullptr &&
              FindAudioRoom(scene, uint32_t(node)) != nullptr,
          "detach trigger retains GI and audio");
    return failed ? 1 : 0;
}
