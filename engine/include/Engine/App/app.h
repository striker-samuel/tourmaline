#pragma once
//
// SDL3 application host: window, event pump (feeds Input), audio engine,
// graphics members (context/swapchain/rendergraph/world).
//
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>
#include <Engine/Audio/audio.h>

#include "input.h"
#include <algorithm>
#include <cassert>
#include <future>
#include <memory>

// SDL3 windowing (forward-declared; .cpp includes <SDL3/SDL_video.h>).
struct SDL_Window;

namespace smvk
{
	class World;
	class RenderGraph;

	class App {
	public:
		Input m_input{};
		audio::AudioEngine m_audio{};
		App() = default;
		virtual ~App();
		virtual bool Initialize() = 0;
		virtual void DrawFrame() = 0;
		// Called while object is still fully constructed, after MainLoop exits
		// and before any members are destroyed. Override to release Holders
		// that depend on the graphics context while GPU is still valid.
		virtual void Shutdown();
		bool Run();
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

		// HOSTED boot (embedded render-service mode): an external host
		// App (same process, same Vulkan context — volk's globals rule
		// out a second instance) drives this App's frame loop manually.
		// The hosted instance borrows the host's graphics members, then
		// runs its own hosted swapchain (offscreen render target),
		// rendergraph and world. Call BEFORE Initialize(); the host
		// then drives: UpdateDeltaTime() + DrawFrame() per its own
		// frame, with device-idle boundaries around the hosted frame
		// (engine globals are per-frame-slot shared state — no overlap).
		// Returns false when this App is already initialized.
		bool RequestHostedBoot(App& host, Extent2D targetExtent,
			Format targetFormat);
		bool IsHosted() const { return hostedBoot_; }
		// The hosted render target (offscreen "surface" view) for
		// external composition. Empty when not hosted.
		ImageViewHandle HostedTargetView() const;

	protected:
#pragma region ===[ App / Core State ]===
		float delta_time_s = .0f;
		float time_since_start_s = .0f;
#pragma endregion

#pragma region ===[ Graphics State ]===
		// Graphics members live in App so they are destroyed BEFORE window.
		std::unique_ptr<ResourceManager> m_context;
		std::unique_ptr<Swapchain> m_swapchain;
		std::unique_ptr<RenderGraph> m_rendergraph;
		std::unique_ptr<World> m_world;
		void WaitForDeviceIdle();
#pragma endregion

#pragma region ===[ SDL3 / Window State ]===
		SDL_Window* window = nullptr;
		bool windowShouldClose_ = false;
		bool relativeMouseMode_ = false;
		int frameBudget_ = 0;   // benchmark frame budget (0 = interactive)
		int benchFrames_ = 0;   // measured frames (post warmup)
		int benchWarmup_ = 0;   // warmup frames skipped from stats
		double benchAccum_ = 0.0;
		// Hosted-boot state (RequestHostedBoot): borrowed graphics +
		// an offscreen target instead of a window surface. The host
		// drives the loop; Run() is never called.
		bool hostedBoot_ = false;
		bool hostedInitialized_ = false;
		App* hostedHost_ = nullptr;
		Extent2D hostedExtent_{};
		Format hostedFormat_ = Format::Undefined;
		// borrowed-context marker (no-op deleter) — Shutdown/teardown
		// must not destroy or quit anything the host owns.
		bool contextBorrowed_ = false;
		// Drains the SDL queue into m_input; returns false when quit requested.
		// Also forwards events to the ImGui SDL3 backend when hooked.
		bool PollEvents();
		// Optional ImGui event hook (set by scenes owning an ImGuiOverlay).
		// Signature matches ImGui_ImplSDL3_ProcessEvent(const SDL_Event*).
		using ImGuiEventFn = bool(*)(const void* sdlEvent);
		ImGuiEventFn imguiEventHook_ = nullptr;
#pragma endregion

#pragma region Events
		virtual void EarlyUpdate() {}
		virtual void Update() {}
		virtual void LateUpdate() {}
		virtual void OnFramebufferResized(int width, int height);
#pragma endregion
	};

	// Backward-compat aliases – old code used IApp / AppImpl (VulkanApp)
	using IApp = App;
	using AppImpl = App;

};
