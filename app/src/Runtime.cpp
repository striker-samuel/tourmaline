// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

// Runtime — see Runtime.h (the application-facing runtime: the
// context, the game element, the packaged-default entry). A thin
// adapter: everything real (the session, the elements, the gate) is
// the landed layers' surface.

#include "Runtime.h"

#include <Engine/App/app.h> // tour::App's full type (the random() defn)

#include <cstring>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h> // GetModuleFileNameA (the exe-relative manifest)
#else
#include <unistd.h> // readlink (/proc/self/exe)
#endif

namespace app {

// ---- Context -------------------------------------------------------------

Context::Context(app::ProjectRuntime& session, tour::App& host,
	engine::WorldRenderer& renderer,
	std::shared_ptr<tour::World*> worldHandle)
	: m_session(session)
	, m_host(host)
	, m_renderer(renderer)
	, m_worldHandle(std::move(worldHandle)) {}

app::ProjectScenes& Context::scenes() {
	// Lazily built at first use (one per context): the scene service
	// over the SAME session every other content read resolved through
	// (the world comes per-load; the m_vfs is the protocol surface
	// only — see content(8)).
	if (!m_scenes)
		m_scenes = std::make_unique<app::ProjectScenes>(m_session,
			m_vfs);
	return *m_scenes;
}

app::AudioGraphs& Context::audio() {
	// Lazily built at first use (one per context): the audio-graph
	// service pairing the shared session (graph text + sound bytes)
	// with the host engine's AudioEngine (the instance registry).
	if (!m_audio)
		m_audio = std::make_unique<app::AudioGraphs>(m_session,
			m_host);
	return *m_audio;
}

tour::RandomService& Context::random() {
	// The engine's session-owned gameplay RNG service (tour::App
	// members outlive every game element).
	return m_host.Random();
}

// ---- GameElement -----------------------------------------------------------

GameElement::GameElement(GameBehaviour* game,
	std::shared_ptr<tour::World*> worldHandle,
	std::shared_ptr<app::ProjectRuntime> session,
	tour::App& host, engine::WorldRenderer& renderer)
	: m_game(game)
	, m_worldHandle(std::move(worldHandle))
	, m_session(std::move(session))
	, m_host(host)
	, m_renderer(renderer) {}

bool GameElement::onAttach() {
	// The renderer attached before this element stacked (the attach
	// order), so the world + the session are live — the game's start
	// sees exactly what the sample's old element saw, one hook fewer.
	if (!m_game)
		return true;
	m_context = std::make_unique<Context>(*m_session, m_host,
		m_renderer, m_worldHandle);
	m_game->onStart(*m_context);
	return true;
}

void GameElement::onUpdate(float dt) {
	if (m_context && m_game)
		m_game->onUpdate(*m_context, dt);
}

// ---- the packaged default entry --------------------------------------------

std::vector<const char*> ArgsWithDefaultEntry(int argc,
	const char* argv[], const std::string& defaultManifest) {
	std::vector<const char*> args(argv, argv + argc);
	if (defaultManifest.empty())
		return args;
	bool hasEntry = false;
	for (int i = 1; i < argc; ++i)
		if (std::strncmp(argv[i], "--pak", 5) == 0
				|| std::strncmp(argv[i], "--project", 9) == 0)
			hasEntry = true;
	if (hasEntry)
		return args;
	// The packaged entry: the appmanifest beside the exe (the build
	// publishes it there; its paks resolve manifest-relative so the
	// exe+content pair moves as one). The static string keeps the
	// pushed pointer valid for the parse (argv-outlives-scope: the
	// args vector dies at RunGame's return, the parse consumed it
	// synchronously inside).
	char exePath[1024] = {};
#ifdef _WIN32
	GetModuleFileNameA(nullptr, exePath, DWORD(sizeof(exePath)));
#else
	const ssize_t n = readlink("/proc/self/exe", exePath,
		sizeof(exePath) - 1);
	if (n > 0)
		exePath[n] = '\0';
#endif
	namespace fs = std::filesystem;
	static const std::string manifestArg =
		(fs::path(exePath).parent_path() / defaultManifest)
			.lexically_normal()
			.generic_string();
	args.push_back("--project");
	args.push_back(manifestArg.c_str());
	return args;
}

} // namespace app
