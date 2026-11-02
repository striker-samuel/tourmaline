// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

#include "AudioGraphs.h"

#include <Engine/App/app.h> // tour::App (m_audio — the engine's registry)
#include <Engine/Asset/appmanifest.h> // the catalog kind bytes

#include <cstdio>
#include <iostream>

namespace app {

// The caches + the resolver context. Everything here lives on the
// GAME thread (the engine's CreateInstance calls the resolver BEFORE
// handing anything to the mixer thread — pak reads + WAV decode never
// touch the audio thread).
struct AudioGraphs::Lane {
    tour::audio::AudioEngine* engine = nullptr;
    app::ProjectRuntime* session = nullptr;
    // The parsed doc per graph id (immutable + shared once parsed).
    std::map<uint64_t, std::shared_ptr<const tour::asset::AudioGraphDoc>>
        docs;
    // The decoded clip per sound VFS path (shared across instances).
    std::map<std::string, tour::audio::ClipHandle> clips;
};

AudioGraphs::AudioGraphs(app::ProjectRuntime& session, tour::App& host)
    : m_session(session)
    , m_host(host)
    , m_lane(std::make_unique<Lane>()) {
    m_lane->engine = &host.m_audio;
    m_lane->session = &m_session;
}

AudioGraphs::~AudioGraphs() = default;

tour::audio::ClipHandle AudioGraphs::ResolveClip(void* user,
    const char* soundVfsPath) {
    auto* lane = static_cast<Lane*>(user);
    if (!lane || !lane->engine || !lane->session || !soundVfsPath)
        return {};
    // Cached? (Every instance of a graph reuses the same decoded
    // bytes — one decode per sound path per session.)
    const std::string path(soundVfsPath);
    if (const auto it = lane->clips.find(path); it != lane->clips.end())
        return it->second;
    // Read the Sound-kind bytes BY PATH through the session (the
    // convergence contract: the same provider selection the id-form
    // read uses — a mod/override serving the path wins).
    std::string rerr;
    auto bytes = lane->session->ReadAssetByPath(
        smvk::kCatalogKindSound, path, rerr);
    if (!bytes || bytes->empty()) {
        std::cerr << "[Audio] graph sound '" << path
           	<< "' is not served by the session: " << rerr << "\n";
        return {};
    }
    const tour::audio::ClipHandle clip =
        lane->engine->LoadWavFromMemory(bytes->data(), bytes->size());
    if (!clip.valid()) {
        std::cerr << "[Audio] graph sound '" << path
           	<< "' failed to decode: "
           	<< lane->engine->LastError() << "\n";
        return {};
    }
    lane->clips.emplace(path, clip);
    return clip;
}

tour::audio::AudioInstance AudioGraphs::create(uint64_t graphId,
    std::string* err) {
    const auto idHex = [](uint64_t v) {
        char b[32];
        std::snprintf(b, sizeof(b), "0x%llx",
            static_cast<unsigned long long>(v));
        return std::string(b);
    };
    const auto fail = [&](const std::string& m)
        -> tour::audio::AudioInstance {
        if (err)
            *err = m;
        std::cerr << "[Audio] graph create failed: " << m << "\n";
        return {};
    };
    // The record gate: the id must name an AudioGraph-kind asset (a
    // wrong-kind id fails loudly instead of parsing some other kind's
    // bytes).
    const tour::asset::ContentRecord* record = m_session.Asset(graphId);
    if (!record)
        return fail("no asset record for id " + idHex(graphId));
    if (record->kind != smvk::kCatalogKindAudioGraph)
        return fail("asset " + idHex(graphId)
            + " is not an audiograph (kind "
            + std::string(smvk::CatalogKindToString(record->kind))
            + ")");
    // The parsed doc (cached — one parse serves every instance).
    std::shared_ptr<const tour::asset::AudioGraphDoc> doc;
    if (const auto it = m_lane->docs.find(graphId);
        it != m_lane->docs.end()) {
        doc = it->second;
    } else {
        std::string rerr;
        auto bytes = m_session.ReadAsset(graphId, rerr);
        if (!bytes || bytes->empty())
            return fail("the audiograph bytes are unserved: " + rerr);
        const std::string text(bytes->begin(), bytes->end());
        auto parsed =
            std::make_shared<tour::asset::AudioGraphDoc>();
        std::string perr;
        if (!tour::asset::ParseAudioGraphText(text, *parsed, perr))
            return fail("the audiograph failed to parse (" + perr
                + " — a cook-time gate should have caught this)");
        doc = parsed;
        m_lane->docs.emplace(graphId, doc);
    }
    // The engine build (clips resolve through the session on THIS
    // thread; the mixer thread only constructs the node set).
    std::string createErr;
    const auto handle = m_lane->engine->CreateInstance(*doc,
        &AudioGraphs::ResolveClip, m_lane.get(), &createErr);
    if (!handle)
        return fail(createErr.empty()
            ? "the engine refused the instance create" : createErr);
    if (err)
        err->clear();
    return tour::audio::AudioInstance(*m_lane->engine, handle);
}

} // namespace app
