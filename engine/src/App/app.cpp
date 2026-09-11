#include <iostream>
#include <chrono>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>

#include <Engine/core.h>
#include <Engine/App/app.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/platform.h>
#include <Engine/Profile/profile.h>


bool smvk::App::Run() {
	bool result = Initialize();
	assert(result && "Failed to initialize");
	MainLoop();
	// Ensure GPU idle and derived Holders are released while object is
	// still fully constructed (all members alive). This avoids destroying
	// VkImage/Buffer-backed Holders after the device/allocator is gone.
	Shutdown();
	return true;
};

void smvk::App::UpdateDeltaTime() {

	static auto lastFrameTime = std::chrono::high_resolution_clock::now();
	static auto startTime = std::chrono::high_resolution_clock::now();
	auto currentTime = std::chrono::high_resolution_clock::now();

	time_since_start_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - startTime).count();

	delta_time_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - lastFrameTime).count();

	lastFrameTime = currentTime;
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
		if (m_input.onSDLEvent(e))
			windowShouldClose_ = true;
	}
	if (m_input.isQuitRequested())
		windowShouldClose_ = true;
	if (m_input.isKeyHeld(SDL_SCANCODE_ESCAPE))
		windowShouldClose_ = true;
	return !windowShouldClose_;
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
			} else {
				benchAccum_ += delta_time_s;
				if (++benchFrames_ >= frameBudget_) {
					const double avgMs = benchAccum_ / benchFrames_ * 1000.0;
					std::printf("[perf] avg frame %.4f ms over %d frames\n",
						avgMs, benchFrames_);
					std::printf("[perf] sections poll %.3f ms game %.3f ms draw %.3f ms\n",
						benchPollMs / benchFrames_, benchGameMs / benchFrames_,
						benchDrawMs / benchFrames_);
					std::fflush(stdout);
					windowShouldClose_ = true;
				}
			}
		}
	}
}
