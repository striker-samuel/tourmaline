// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
//
// SDL3 application host (nvapp::Application-style): window, event pump
// (feeds Input), audio engine, graphics members (context/swapchain/
// rendergraph/world) and a stack of IAppElements. The ELEMENT stack is
// the renderer extension point — hosts subclass for shell customization
// (EditorApp, the samples), renderers are elements (WorldRenderer).
//
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>
#include <Engine/App/Element.h>
#include <Engine/Audio/audio.h>
#include <Engine/Memory/memory.h>

#include "input.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <future>
#include <memory>
#include <vector>

// SDL3 windowing (forward-declared; .cpp includes <SDL3/SDL_video.h>).
struct SDL_Window;

namespace smvk
{
	class World;
	class RenderGraph;

class SdlInputBackend;

class App {
public:
	Input m_input{};
	// Platform input bridge (SDL3 translation + gamepad handle lifetime).
	// Owned here so handles close while SDL is alive; hosts never see it.
	std::unique_ptr<SdlInputBackend> m_sdlInput;
	audio::AudioEngine m_audio{};
	App();
	// The pasted-pattern contract: Shutdown detaches + destroys the
	// element stack while the graphics members are alive — an assert
	// here (debug builds) catches a forgotten Shutdown.
	virtual ~App();

	// Shell customization. Initialize boots the windowed graphics
	// members (SDL window, context, swapchain, rendergraph, world);
	// Run drives the loop (attach elements -> pump frames -> shutdown);
	// Shutdown detaches the element stack while everything is alive.
	// Hosts override for custom boot/teardown or a custom DrawFrame
	// (EditorApp); the DEFAULT DrawFrame below is the element pump.
	virtual bool Initialize();
	virtual void Shutdown();
	virtual bool Run();
	void MainLoop();
	void UpdateDeltaTime();

	// Window helpers (SDL3).
	SDL_Window* GetWindow() const { return window; }
	bool ShouldClose() const { return windowShouldClose_; }
	void RequestClose() { windowShouldClose_ = true; }
	void SetRelativeMouseMode(bool enabled);
	bool GetRelativeMouseMode() const { return relativeMouseMode_; }
	// Benchmark: auto-quit after N frames and report average frame time
	// (`--bench N` in App/main.cpp). 0 = run interactively (default).
	void SetFrameBudget(int n) { frameBudget_ = n; }
	int GetFrameBudget() const { return frameBudget_; }
	// True while the measured window runs (post warmup) — element-side
	// bench accounting (WorldRenderer's [perf] phase split) keys on it.
	bool BenchMeasuring() const { return frameBudget_ > 0 && benchWarmup_ >= 10; }
	bool BenchFinalFrame() const { return frameBudget_ > 0 && benchFrames_ + 1 >= frameBudget_; }

	// ------------------------------------------------------------------
	// ELEMENT STACK (nvapp-style): renderers plug in as IAppElements.
	// AddElement BEFORE Run(); Run attaches (onAttach) after Initialize,
	// the default DrawFrame pump drives update/pre-render/render between
	// the host's acquire and present, and Shutdown detaches (onDetach,
	// reverse order) while the graphics members are still alive.
	// Elements render into the host's swapchain image through the
	// ElementFrame handed to onRender — never their own acquire/present
	// (see Engine/App/Element.h for the full contract).
	// ------------------------------------------------------------------
	void AddElement(const std::shared_ptr<engine::IAppElement>& element);
	// Safely free GPU resources: the fn runs after the frames in flight
	// retire (the pump drains the submitting flight's queue right after
	// its acquire fence wait) — safe against in-flight use.
	void SubmitResourceFree(std::function<void()>&& func);
	// Swapchain sync-owner controls (the element contract: elements never
	// Recreate — they observe rebuilds through onResize). The requested
	// present mode applies at the next frame's rebuild check.
	void SetPresentMode(PresentMode mode) { m_presentMode = mode; }
	PresentMode GetPresentMode() const { return m_presentMode; }
	void RequestSwapchainRebuild() {
		if (m_swapchain)
			m_swapchain->SetFramebufferResized(true);
	}
	// Last acquire wait (ms): the element-side CPU/wait timing split
	// (WorldRenderer's [perf] lines) reads the host's acquire idle.
	double GetLastAcquireWaitMs() const { return m_lastAcquireWaitMs; }

	// Optional ImGui event hook (set by scenes owning an ImGuiOverlay).
	// Signature matches ImGui_ImplSDL3_ProcessEvent(const SDL_Event*).
	using ImGuiEventFn = bool(*)(const void* sdlEvent);
	void SetImGuiEventHook(ImGuiEventFn fn) { imguiEventHook_ = fn; }

	// ---- Graphics-member access (elements + hosts): null before boot ----
	ResourceManager* GetResourceManager() const { return m_context.get(); }
	Swapchain* GetSwapchain() const { return m_swapchain.get(); }
	RenderGraph* GetRendergraph() const { return m_rendergraph.get(); }
	// Stall until the device drains (frame-boundary contract: teardown,
	// readbacks, structural rebuilds).
	void WaitForDeviceIdle();

protected:
#pragma region ===[ App / Core State ]===
	float delta_time_s = .0f;
	float time_since_start_s = .0f;
	// REAL wall delta (pre-bench-pin): the perf accounting averages THIS
	// (a true wall-time metric) while delta_time_s is pinned to a fixed
	// 60 Hz step during --bench for capture determinism (spawn quotas,
	// animation clocks). Measuring the pin would report 16.67 ms forever.
	double real_delta_time_s = 0.0;
#pragma endregion

#pragma region ===[ Graphics State ]===
	// Graphics members live in App so they are destroyed BEFORE window.
	// Linear process-owned (dtor-only deleter; never freed). The CONTEXT
	// + SWAPCHAIN are the frame-sync owners — elements borrow them
	// through the ElementFrame / onAttach host accessors; the graph and
	// world below are the HOST's own (EditorApp's scene world; the
	// rendergraph its own passes record into). Elements own theirs.
	mem::LinearPtr<ResourceManager> m_context;
	mem::LinearPtr<Swapchain> m_swapchain;
	mem::LinearPtr<RenderGraph> m_rendergraph;
	mem::LinearPtr<World> m_world;
#pragma endregion

#pragma region ===[ Element Stack ]===
	// Owned elements (nvapp-style shared ownership; detach at Shutdown).
	// The default DrawFrame pump drives them; custom-frame hosts
	// (EditorApp) drive their own flow and keep this stack empty.
	std::vector<std::shared_ptr<engine::IAppElement>> m_elements;
	// Per-flight deferred-free queues (SubmitResourceFree): the pump
	// drains the acquired flight's queue after its fence wait.
	std::vector<std::vector<std::function<void()>>> m_resourceFreeQueue;
	uint32_t m_freeQueueRingCurrent = 0;
	void ResetFreeQueue(uint32_t size);
	void DrainFreeQueue(uint32_t flight);
	PresentMode m_presentMode = PresentMode::Fifo; // requested; pump applies
	double m_lastAcquireWaitMs = 0.0;
	// DEFAULT element pump (overridable host frame): present-mode drift +
	// resize rebuilds (elements observe via onResize), acquire ->
	// onPreRender/onRender stack -> present, deferred frees.
	virtual void DrawFrame();
#pragma endregion

#pragma region ===[ SDL3 / Window State ]===
	SDL_Window* window = nullptr;
	bool windowShouldClose_ = false;
	bool relativeMouseMode_ = false;
	int frameBudget_ = 0;   // benchmark frame budget (0 = interactive)
	int benchFrames_ = 0;   // measured frames (post warmup)
	int benchWarmup_ = 0;   // warmup frames skipped from stats
	double benchAccum_ = 0.0;
	// Drains the SDL queue through the input bridge into m_input; returns
	// false when quit requested. Also forwards raw events to the ImGui SDL3
	// backend when hooked (that hook stays SDL-typed: it IS the ImGui
	// backend boundary).
	bool PollEvents();
	ImGuiEventFn imguiEventHook_ = nullptr;
#pragma endregion

#pragma region Events
	virtual void EarlyUpdate() {}
	virtual void Update() {}
	virtual void LateUpdate() {}
	virtual void OnFramebufferResized(int width, int height);
	// Backend-neutral input event tap (every event PollEvents pumps, after
	// the ImGui hook, before the Input consume). Hosts that need non-input
	// events — e.g. the editor's OS drag-and-drop (input::EventType::FileDrop)
	// — override this instead of duplicating the pump. Default: no-op.
	virtual void OnInputEvent(const input::Event&) {}
#pragma endregion
	};
};
