// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <iostream>
#include <chrono>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include <Engine/App/input_sdl3.h>

#include <Engine/core.h>
#include <Engine/App/app.h>
//#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Memory/arena.h>
#include <Engine/platform.h>
#include <Engine/Profile/profile.h>


bool smvk::App::Run() {
	if (!Initialize()) {
		std::cerr << "[App] initialization failed — exiting\n";
		Shutdown();
		return false;
	}
	// Attach the stack AFTER boot: elements see live graphics members
	// through their onAttach (context/swapchain resolve there). A failed
	// attach aborts the run loudly (Shutdown still detaches the rest
	// while everything is alive).
	for (const auto& element : m_elements) {
		if (!element)
			continue;
		if (!element->onAttach()) {
			std::cerr << "[App] element '" << element->ElementName()
				<< "' failed to attach. exiting\n";
			Shutdown();
			return false;
		}
	}
	MainLoop();
	// Ensure GPU idle and elements are released while object is
	// still fully constructed (all members alive). This avoids destroying
	// VkImage/Buffer-backed Holders after the device/allocator is gone.
	Shutdown();
	return true;
};

void smvk::App::AddElement(const std::shared_ptr<engine::IAppElement>& element) {
	if (element)
		m_elements.push_back(element);
}

void smvk::App::SubmitResourceFree(std::function<void()>&& func) {
	if (!func)
		return;
	// Lazy ring: sized to the live swapchain's flight count on first use.
	const uint32_t flights = m_swapchain ? m_swapchain->framesInFlight() : 1u;
	if (m_resourceFreeQueue.size() != flights)
		ResetFreeQueue(flights);
	m_resourceFreeQueue[m_freeQueueRingCurrent].push_back(std::move(func));
}

void smvk::App::ResetFreeQueue(uint32_t size) {
	m_resourceFreeQueue.clear();
	m_resourceFreeQueue.resize(size ? size : 1u);
	m_freeQueueRingCurrent = 0;
}

void smvk::App::DrainFreeQueue(uint32_t flight) {
	if (m_resourceFreeQueue.empty())
		return;
	if (flight >= m_resourceFreeQueue.size())
		flight = 0; // flight-count drift (rebuild): drain slot 0
	for (auto& fn : m_resourceFreeQueue[flight])
		fn();
	m_resourceFreeQueue[flight].clear();
}

void smvk::App::UpdateDeltaTime() {

	static auto lastFrameTime = std::chrono::high_resolution_clock::now();
	static auto startTime = std::chrono::high_resolution_clock::now();
	auto currentTime = std::chrono::high_resolution_clock::now();

	time_since_start_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - startTime).count();

	delta_time_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - lastFrameTime).count();

	lastFrameTime = currentTime;

	// Bench determinism: pin the element dt to a fixed 60 Hz step while
	// measuring. The real dt varies run to run (vsync jitter, uncapped
	// frame pacing) and EVERYTHING animated rides it - particle spawn
	// quotas + integration, the light-crowd orbit clock, bench_move
	// flight, scene anims on the element clock - so captures were
	// run-dependent whenever anything moved (observed: dust/rain benches
	// differed byte-wise between identical runs). Fixed dt makes the
	// measured frames pixel-comparable again. The bench WALL-time
	// accounting below is untouched (it accumulates the real wall delta
	// captured here - measuring the pin would report 16.67 ms forever).
	real_delta_time_s = static_cast<double>(delta_time_s);
	if (frameBudget_ > 0)
		delta_time_s = 1.0f / 60.0f;
}

void smvk::App::SetRelativeMouseMode(bool enabled) {
	relativeMouseMode_ = enabled;
	if (window) {
		if (!SDL_SetWindowRelativeMouseMode(window, enabled)) {
			std::cerr << "[App] SDL_SetWindowRelativeMouseMode failed: "
				<< SDL_GetError() << "\n";
		}
	}
}

	bool smvk::App::PollEvents() {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
		// Let ImGui consume UI events first (hook set by ImGuiOverlay::Init).
		if (imguiEventHook_)
			imguiEventHook_(&e);
		if ((e.type == SDL_EVENT_WINDOW_RESIZED ||
				e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) &&
			window && e.window.windowID == SDL_GetWindowID(window)) {
			OnFramebufferResized(e.window.data1, e.window.data2);
		}
		// Normalize at the bridge: hosts + Input see only input::Event.
		input::Event inputEvent;
		if (m_sdlInput && m_sdlInput->Translate(e, inputEvent)) {
			OnInputEvent(inputEvent);
			if (m_input.onEvent(inputEvent))
				windowShouldClose_ = true;
		}
	}
	if (m_input.isQuitRequested())
		windowShouldClose_ = true;
	// Held-Escape quit: the base host's game flow. The EDITOR overrides
	// AllowEscQuit to false — Esc must never kill an editing session
	// outright (it dismisses modals/popups via ImGui instead).
	if (AllowEscQuit() && m_input.isKeyHeld(input::Key::Escape))
		windowShouldClose_ = true;
	return !windowShouldClose_;
}

void smvk::App::DrawFrame() {
	// DEFAULT element pump (nvapp-style host frame): sync stays HERE —
	// present-mode drift + resize rebuilds (elements observe via
	// onResize), one acquire, the stack's pre-render/render record into
	// the host's frame, one present, then the deferred frees drain.
	if (!m_swapchain)
		return;
	if (m_presentMode != m_swapchain->GetPresentMode()) {
		m_swapchain->SetPresentMode(m_presentMode);
		m_swapchain->Recreate();
		const auto ext = m_swapchain->GetExtent();
		for (const auto& element : m_elements)
			if (element)
				element->onResize(ext);
		m_swapchain->ClearFramebufferResized();
	}
	if (m_swapchain->FramebufferResized()) {
		m_swapchain->Recreate();
		const auto ext = m_swapchain->GetExtent();
		for (const auto& element : m_elements)
			if (element)
				element->onResize(ext);
		m_swapchain->ClearFramebufferResized();
	}
	const auto acquireStart = std::chrono::steady_clock::now();
	auto fb = m_swapchain->AcquireFramebuffer();
	m_lastAcquireWaitMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - acquireStart).count();
	if (m_resourceFreeQueue.size() != m_swapchain->framesInFlight())
		ResetFreeQueue(m_swapchain->framesInFlight());
	DrainFreeQueue(fb.frameIndex);
	m_freeQueueRingCurrent = fb.frameIndex % uint32_t(m_resourceFreeQueue.size());
	const auto ext = m_swapchain->GetExtent();
	engine::ElementFrame frame{ &fb, ext, engine::LayerRect::Full() };
	for (const auto& element : m_elements)
		if (element)
			element->onPreRender(frame);
	for (const auto& element : m_elements)
		if (element)
			element->onRender(frame);
	m_swapchain->EndAndPresentFramebuffer(fb);
}

void smvk::App::MainLoop() {
	SMVK_PROFILE_THREAD("MainThread");
	windowShouldClose_ = false;
	double benchPollMs = 0.0, benchGameMs = 0.0, benchDrawMs = 0.0;
	while (!windowShouldClose_) {
		SMVK_PROFILE_FRAME("MainThread");
		auto t0 = std::chrono::steady_clock::now();
		{
			SMVK_PROFILE_SCOPE_COLOR("PollEvents", Optick::Color::Ivory);
			m_input.beginFrame();
			PollEvents();
		}
		UpdateDeltaTime();
		// Frame-arena rewind at the frame boundary: everything frame-
		// allocated last frame is dead by definition (the frame presented;
		// the acquire fence gates reuse). Hosts + elements rely on this —
		// never reset the arena themselves.
		smvk::ResetFrameArena();
		auto t1 = std::chrono::steady_clock::now();
		{
			SMVK_PROFILE_CATEGORY("EarlyUpdate", Optick::Category::GameLogic);
			EarlyUpdate();
		}
		{
			SMVK_PROFILE_CATEGORY("Update", Optick::Category::GameLogic);
			Update();
		}
		{
			SMVK_PROFILE_CATEGORY("LateUpdate", Optick::Category::GameLogic);
			LateUpdate();
		}
		// Element CPU update (the renderer extension point): runs on the
		// host's clock — hosts that never attach elements pay one empty
		// vector walk.
		for (const auto& element : m_elements)
			if (element)
				element->onUpdate(delta_time_s);
		auto t2 = std::chrono::steady_clock::now();
		{
			SMVK_PROFILE_CATEGORY("DrawFrame", Optick::Category::Rendering);
			DrawFrame();
		}
		auto t3 = std::chrono::steady_clock::now();
		if (frameBudget_ > 0 && benchWarmup_ >= 10) {
			using ms = std::chrono::duration<double, std::milli>;
			benchPollMs += ms(t1 - t0).count();
			benchGameMs += ms(t2 - t1).count();
			benchDrawMs += ms(t3 - t2).count();
		}
		if (frameBudget_ > 0) {
			// Benchmark accounting: skip warmup (pipeline creation hitches),
			// then average delta_time_s (present throttle included).
			if (benchWarmup_ < 10) {
				++benchWarmup_;
				if (benchWarmup_ == 10) {
					// Start an Optick capture for the measured window so the
					// CPU+GPU breakdown can be inspected in Optick Viewer.
					// Opt-in only (--use_optick): the capture session is
					// RenderDoc-hostile.
					if (smvk::profiler::Enabled())
						OPTICK_START_CAPTURE();
				}
			} else {
				// WALL delta (real_delta_time_s): the perf metric must
				// report true frame cost; delta_time_s is the pinned sim
				// step during benches (see UpdateDeltaTime).
				benchAccum_ += real_delta_time_s;
				if (++benchFrames_ >= frameBudget_) {
					const double avgMs = benchAccum_ / benchFrames_ * 1000.0;
					std::printf("[perf] avg frame %.4f ms over %d frames\n",
						avgMs, benchFrames_);
					std::printf("[perf] sections poll %.3f ms game %.3f ms draw %.3f ms\n",
						benchPollMs / benchFrames_, benchGameMs / benchFrames_,
						benchDrawMs / benchFrames_);
					std::fflush(stdout);
					if (smvk::profiler::Enabled()) {
						OPTICK_STOP_CAPTURE();
						const bool benchSaved = ::Optick::SaveCapture("bench.opt");
						std::printf("[perf] optick capture %s to bench.opt\n",
							benchSaved ? "saved" : "FAILED");
					}
					windowShouldClose_ = true;
				}
			}
		}
	}
}
