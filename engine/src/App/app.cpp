// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include <Engine/App/input_sdl3.h>

#include <Engine/App/app.h>
#include <Engine/core.h>
// #include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Memory/arena.h>
#include <Engine/Profile/profile.h>
#include <Engine/Utils/nsight_gfx.h>
#include <Engine/platform.h>

bool tour::App::Run() {
    // Nsight Graphics SDK: initialize BEFORE the graphics context exists
    // (the SDK contract). No-op in builds without TOURMALINE_NGFX_SDK.
    tour::nsight_gfx::Initialize();
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
    if (m_runFinalizer) {
        const std::string result = m_runFinalizer();
        if (!result.empty() && m_inputTapeError.empty())
            m_inputTapeError = "snapshot sequence: " + result;
    }
    if (m_inputTapeMode == InputTapeMode::Replay &&
        m_inputTapeFrame != m_inputTape.Frames().size() && m_inputTapeError.empty())
        m_inputTapeError = "replay interrupted before all recorded ticks were consumed";
    if (m_inputTapeError.empty() && m_inputTapeMode == InputTapeMode::Record &&
        !m_inputTapePath.empty() && !m_inputTape.Save(m_inputTapePath, m_inputTapeError))
        std::cerr << "[InputTape] save failed: " << m_inputTapeError << "\n";
    if (!m_inputTapeError.empty())
        std::cerr << "[InputTape] " << m_inputTapeError << "\n";
    const bool tapeOk = m_inputTapeError.empty();
    // Ensure GPU idle and elements are released while object is
    // still fully constructed (all members alive). This avoids destroying
    // VkImage/Buffer-backed Holders after the device/allocator is gone.
    Shutdown();
    return tapeOk;
};

bool tour::App::ConfigureInputRecording(std::string path, std::string& error) {
    if (path.empty() || m_inputTapeMode != InputTapeMode::Live) {
        error = "record path missing or input mode already configured";
        return false;
    }
    m_inputTape.Clear();
    m_inputTapePath = std::move(path);
    m_inputTapeMode = InputTapeMode::Record;
    return true;
}

bool tour::App::ConfigureInputReplay(std::string path, std::string& error) {
    if (path.empty() || m_inputTapeMode != InputTapeMode::Live) {
        error = "replay path missing or input mode already configured";
        return false;
    }
    if (!m_inputTape.Load(path, error))
        return false;
    m_inputTapePath = std::move(path);
    m_inputTapeMode = InputTapeMode::Replay;
    return true;
}

bool tour::App::ConfigureSnapshotInputCapture(std::string& error) {
    (void)error;
    if (m_inputTapeMode == InputTapeMode::Live) {
        m_inputTape.Clear();
        m_inputTapeMode = InputTapeMode::Record;
        m_inputTapePath.clear(); // no sidecar; all events go into PNGs
    }
    return true;
}

void tour::App::AddElement(const std::shared_ptr<engine::IAppElement>& element) {
    if (element)
        m_elements.push_back(element);
}

void tour::App::SubmitResourceFree(std::function<void()>&& func) {
    if (!func)
        return;
    // Lazy ring: sized to the live swapchain's flight count on first use.
    const uint32_t flights = m_swapchain ? m_swapchain->framesInFlight() : 1u;
    if (m_resourceFreeQueue.size() != flights)
        ResetFreeQueue(flights);
    m_resourceFreeQueue[m_freeQueueRingCurrent].push_back(std::move(func));
}

void tour::App::ResetFreeQueue(uint32_t size) {
    m_resourceFreeQueue.clear();
    m_resourceFreeQueue.resize(size ? size : 1u);
    m_freeQueueRingCurrent = 0;
}

void tour::App::DrainFreeQueue(uint32_t flight) {
    if (m_resourceFreeQueue.empty())
        return;
    if (flight >= m_resourceFreeQueue.size())
        flight = 0; // flight-count drift (rebuild): drain slot 0
    for (auto& fn : m_resourceFreeQueue[flight])
        fn();
    m_resourceFreeQueue[flight].clear();
}

void tour::App::UpdateDeltaTime() {
    const auto currentTime = std::chrono::steady_clock::now();
    if (!m_clockInitialized) {
        m_clockStart = currentTime;
        m_lastFrameStart = currentTime;
        m_clockInitialized = true;
        // The first simulation tick needs a valid dt for input recording.
        real_delta_time_s = 0.0;
        delta_time_s = 1.0f / 60.0f;
    } else {
        real_delta_time_s = std::chrono::duration<double>(currentTime - m_lastFrameStart).count();
        m_lastFrameStart = currentTime;
        // Keep wall time for metrics, but do not feed multi-second
        // minimize/resize/GPU stalls directly into variable-step gameplay.
        delta_time_s = static_cast<float>(std::clamp(real_delta_time_s, 1e-6, 0.1));
    }
    time_since_start_s = std::chrono::duration<float>(currentTime - m_clockStart).count();

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
    if (frameBudget_ > 0)
        delta_time_s = 1.0f / 60.0f;
    if (m_inputTapeMode == InputTapeMode::Replay) {
        const auto* frame = m_inputTape.Frame(m_inputTapeFrame);
        if (frame) {
            delta_time_s = static_cast<float>(frame->deltaTimeNs * 1e-9);
            m_inputTapeTimeNs += frame->deltaTimeNs;
            time_since_start_s = static_cast<float>(m_inputTapeTimeNs * 1e-9);
        }
    }
}

void tour::App::SetRelativeMouseMode(bool enabled) {
    relativeMouseMode_ = enabled;
    if (window) {
        if (!SDL_SetWindowRelativeMouseMode(window, enabled)) {
            std::cerr << "[App] SDL_SetWindowRelativeMouseMode failed: " << SDL_GetError() << "\n";
        }
    }
}

bool tour::App::PollEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        // Let ImGui consume UI events first (hook set by ImGuiOverlay::Init).
        if (imguiEventHook_ && m_inputTapeMode != InputTapeMode::Replay)
            imguiEventHook_(&e);
        // ImGui owns input and close events for its native tool windows.
        // The SDL3 backend has already processed them above; do not forward
        // these to the scene camera, input recorder, or host quit path.
        if (imguiEventHook_ && window) {
            SDL_WindowID eventWindow = 0;
            switch (e.type) {
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                eventWindow = e.window.windowID;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                eventWindow = e.motion.windowID;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                eventWindow = e.button.windowID;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                eventWindow = e.wheel.windowID;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                eventWindow = e.key.windowID;
                break;
            case SDL_EVENT_TEXT_INPUT:
                eventWindow = e.text.windowID;
                break;
            default:
                break;
            }
            if (eventWindow != 0 && eventWindow != SDL_GetWindowID(window))
                continue;
        }
        if ((e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) &&
            window && e.window.windowID == SDL_GetWindowID(window)) {
            OnFramebufferResized(e.window.data1, e.window.data2);
        }
        // Normalize at the bridge: hosts + Input see only input::Event.
        input::Event inputEvent;
        if (m_sdlInput && m_sdlInput->Translate(e, inputEvent)) {
            // Only live OS close events survive replay; suppress all
            // physical gameplay input while the tape is authoritative.
            if (m_inputTapeMode == InputTapeMode::Replay) {
                if (inputEvent.type == input::EventType::Quit ||
                    inputEvent.type == input::EventType::WindowClose) {
                    windowShouldClose_ = true;
                    m_replayOsClose = true;
                }
                continue;
            }
            if (m_inputTapeMode == InputTapeMode::Record &&
                !m_inputTape.Record(inputEvent, m_inputTapeError)) {
                windowShouldClose_ = true;
                break;
            }
            OnInputEvent(inputEvent);
            if (m_input.onEvent(inputEvent))
                windowShouldClose_ = true;
        }
    }
    if (m_inputTapeMode == InputTapeMode::Replay && !windowShouldClose_) {
        const auto* frame = m_inputTape.Frame(m_inputTapeFrame);
        if (!frame) {
            m_inputTapeError = "replay exhausted before scheduled tick";
            windowShouldClose_ = true;
        } else {
            for (const auto& event : frame->events) {
                OnInputEvent(event);
                if (m_input.onEvent(event)) {
                    m_inputTapeError = "replay contains an unexpected close event";
                    windowShouldClose_ = true;
                    break;
                }
            }
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

void tour::App::DrawFrame() {
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
    m_lastAcquireWaitMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquireStart)
            .count();
    if (m_resourceFreeQueue.size() != m_swapchain->framesInFlight())
        ResetFreeQueue(m_swapchain->framesInFlight());
    DrainFreeQueue(fb.frameIndex);
    m_freeQueueRingCurrent = fb.frameIndex % uint32_t(m_resourceFreeQueue.size());
    const auto ext = m_swapchain->GetExtent();
    engine::ElementFrame frame{&fb, ext, engine::LayerRect::Full()};
    for (const auto& element : m_elements)
        if (element)
            element->onPreRender(frame);
    for (const auto& element : m_elements)
        if (element)
            element->onRender(frame);
    m_swapchain->EndAndPresentFramebuffer(fb);
    // Nsight Graphics SDK: procedural capture/trace trigger, polled once per
    // frame (1-based). Frame-boundary marking is opt-in via
    // TOURMALINE_NGFX_BOUNDARY for headless/non-present workloads and is not
    // driven here (the App presents, so capture/trace delimit on Present).
    static uint64_t s_nsightFrame = 0;
    tour::nsight_gfx::Poll(++s_nsightFrame);
}

void tour::App::UpdateVrrFrameLimit() {
    if (!m_vrrEnabled || m_manualMaxFps || !window)
        return;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
    const uint32_t target = mode ? FramePacer::VrrTargetFps(mode->refresh_rate) : 0;
    if (target != m_autoVrrFps) {
        m_autoVrrFps = target;
        m_framePacer.SetTargetFps(target);
        if (target)
            std::cout << "[VRR] refresh " << mode->refresh_rate << " Hz, cap " << target
                      << " FPS\n";
        else
            std::cerr << "[VRR] unknown display refresh rate; automatic FPS cap disabled\n";
    }
}

void tour::App::MainLoop() {
    SMVK_PROFILE_THREAD("MainThread");
    windowShouldClose_ = false;
    m_clockInitialized = false;
    m_framePacer.Reset();
    double benchPollMs = 0.0, benchGameMs = 0.0, benchDrawMs = 0.0;
    while (!windowShouldClose_) {
        if (frameBudget_ == 0) {
            UpdateVrrFrameLimit();
            m_framePacer.BeginFrame();
        }
        if (m_inputTapeMode == InputTapeMode::Record &&
            !m_inputTape.BeginFrame(m_inputTapeFrame, m_inputTapeError))
            break;
        SMVK_PROFILE_FRAME("MainThread");
        auto t0 = std::chrono::steady_clock::now();
        {
            SMVK_PROFILE_SCOPE_COLOR("PollEvents", Optick::Color::Ivory);
            m_input.beginFrame();
            PollEvents();
        }
        if (!m_inputTapeError.empty())
            break;
        // Recorded Escape is part of the last simulation frame. Only an
        // *live OS* close interrupts replay before consuming that frame.
        if (m_inputTapeMode == InputTapeMode::Replay && m_replayOsClose)
            break;
        UpdateDeltaTime();
        if (m_inputTapeMode == InputTapeMode::Record) {
            // Record the simulation timestep, not perf wall time.
            const double ns = std::round(double(delta_time_s) * 1e9);
            if (!std::isfinite(ns) || ns <= 0 || ns > 10'000'000'000.0 ||
                !m_inputTape.SetDeltaTime(static_cast<uint64_t>(ns), m_inputTapeError))
                break;
        }
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
        ++m_inputTapeFrame;
        if (m_totalFrameLimit != 0 && m_inputTapeFrame >= m_totalFrameLimit)
            windowShouldClose_ = true;
        if (m_inputTapeMode == InputTapeMode::Replay &&
            m_inputTapeFrame >= m_inputTape.Frames().size())
            windowShouldClose_ = true;
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
                    std::printf("[perf] avg frame %.4f ms over %d frames\n", avgMs, benchFrames_);
                    std::printf("[perf] sections poll %.3f ms game %.3f ms draw %.3f ms\n",
                                benchPollMs / benchFrames_,
                                benchGameMs / benchFrames_,
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
        // Pacing is outside the measured frame and is never used by --bench.
        if (frameBudget_ == 0 && !windowShouldClose_)
            m_framePacer.EndFrame();
    }
}
