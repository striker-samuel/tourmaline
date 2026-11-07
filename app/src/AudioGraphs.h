// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

#pragma once
// THE TYPED-KEY AUDIO-GRAPH LANE (docs/audio_graph.md): the app-side
// facade gameplay reaches through app::Context::audio(). It pairs the
// SHARED content session (graph text + sound bytes, by id and by
// path) with the engine's AudioEngine (the instance registry + the
// miniaudio rendering core) — the engine never knows paks, the
// session never renders.
//
//   auto forest = ctx.audio().create(project::audiographs::forest);
//   forest.play();                       // the authored default cue
//   forest.set("wind", 0.75f);           // an exposed parameter
//   forest.crossFade(                    // an authored cue (by name
//       project::audiocues::forest_storm.cue, 2.0f);  // or ordinal)
//   forest.stop();
//
// The typed AudioGraphKey constants implicitly convert to the raw Id
// this facade takes (the apphost never includes a generated
// project.h); the generated AudioCueKey constants carry their cue
// ORDINAL in `.cue` (the AudioInstance facade also accepts names).
//
// CACHES (session-lifetime, never evicted in v1 — graphs are few and
// the clip decode is the expensive half; a pin/evict surface rides
// the ownership-aware prefetch when one is needed):
//   - the PARSED doc per graph id (one parse, many instances)
//   - the DECODED clip per sound VFS path (shared across instances —
//     every instance of a graph reuses the same decoded bytes)

#include "ProjectRuntime.h"

#include <Engine/Asset/audiograph.h>
#include <Engine/Audio/audio.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace tour {
class App;
}

namespace app {

class AudioGraphs {
  public:
    AudioGraphs(app::ProjectRuntime& session, tour::App& host);
    ~AudioGraphs(); // defined in the .cpp (Lane is opaque here)

    // Creates a runtime instance of the authored graph `graphId` (the
    // generated AudioGraphKey constants): reads the AudioGraph-kind
    // bytes through the session, parses (cached), resolves every
    // source's clip by VFS path (cached) and hands the payload to the
    // engine's mixer thread. Returns a default-constructed (false)
    // AudioInstance + `err` on any failure — a wrong-kind id, an
    // unparsable graph or an unresolvable sound all fail LOUDLY with
    // the named path.
    [[nodiscard]] tour::audio::AudioInstance create(uint64_t graphId, std::string* err = nullptr);

  private:
    // The doc + clip caches + the resolver context (the engine's
    // AudioClipResolver is a plain function pointer + user data).
    struct Lane;
    static tour::audio::ClipHandle ResolveClip(void* user, const char* soundVfsPath);

    app::ProjectRuntime& m_session;
    tour::App& m_host;
    std::unique_ptr<Lane> m_lane;
};

} // namespace app
