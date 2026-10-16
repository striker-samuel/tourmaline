// WorldRendererElement: the control layer over WorldRenderer (see the
// header for the split). Everything here moved from WorldRenderer â€” the
// frame builder no longer polls input, owns keybinds, or drives modes.

#include <Engine/Renderer/WorldRendererElement.h>

#include <cstdio>

namespace engine {

	WorldRendererElement::WorldRendererElement(smvk::App& host)
		: m_host(host)
		, m_input(host.m_input)
		, m_renderer(host) {
	}

	bool WorldRendererElement::onAttach() {
		if (!m_renderer.onAttach())
			return false;
		// The renderer's gamepad CONFIG widgets + footer status read the
		// input through this back-pointer (status display, never control).
		m_renderer.SetInputForUi(&m_input);
		// Start in FPS mode (DISABLED): mouse captured for mouselook; the
		// I key / gamepad Start toggles to UI (NORMAL). The old renderer
		// onAttach did this right after its ImGui init.
		SetUIMode(false);
		return true;
	}

	void WorldRendererElement::onDetach() {
		m_renderer.onDetach();
	}

	void WorldRendererElement::onResize(smvk::Extent2D swapExtent) {
		(void)swapExtent;
		// The host recreated its swapchain â€” the renderer rebuilds its
		// swap views + temporal histories (it never recreates on its own).
		m_renderer.OnSwapchainRecreated();
	}

	void WorldRendererElement::onUpdate(float dt) {
		// Capture readback FIRST (TOURMALINE_CAPTURE / F7): the previous
		// frame's submit has landed by now (its present waited), so the
		// stall is free; the export lands one frame after the capture
		// frame.
		m_renderer.PollCaptureReadback();
		// Renderer clock latch (shader time + camera integration), then
		// the control pass (the old App::MainLoop Update slot).
		m_renderer.Tick(dt);
		Update(dt);
		// Status push for the renderer's panel footer (display only).
		m_renderer.SetUiModeHint(m_uiMode);
	}

	void WorldRendererElement::onRender(ElementFrame& frame) {
		m_renderer.RenderLayerInto(frame);
	}

	void WorldRendererElement::Update(float dt) {
		HandleShortcuts();
		// Headless crosshair splat (TOURMALINE_DECAL_AUTOSPLAT=<frame>):
		// same path as the J key, fired once past the frame index so
		// capture/A-B runs exercise the pick-BVH + raycast glue.
		if (m_renderer.m_decalAutoSplatAtFrame >= 0
			&& (int64_t)m_renderer.m_frameCounter
			>= m_renderer.m_decalAutoSplatAtFrame) {
			m_renderer.m_decalAutoSplatAtFrame = -1;
			m_renderer.SpawnDecalAtCrosshair();
		}
		if (!m_uiMode && !m_disableControl) {
			UpdateCamera(dt);
		}
		// Bench flight (--bench_move): constant-velocity forward
		// integration, straight along the spawn forward vector. Runs
		// past warmup only (warmup stays at the spawn pose so every
		// measured run starts identically) and stacks on --disable_control
		// (frozen inputs, deterministic flight).
		if (m_benchMoveSpeed > 0.0f && m_host.GetFrameBudget() > 0
			&& m_host.BenchMeasuring() && !m_uiMode) {
			auto& cam = m_renderer.CameraPositioner();
			cam.cameraPosition_ += cam.getForward() * (m_benchMoveSpeed * dt);
		}
	}

	// Global feature shortcuts (work in both FPS and UI modes so A/B
	// comparisons don't require opening the panel; see the footer readout
	// in the renderer's panel for the full list). WASDQE/Space/Shift/Ctrl
	// are reserved for camera movement and are never used here. Keys live
	// in Engine/App/keybindings.h â€” this handler consumes semantic names,
	// never raw GLFW_KEY_* reads. Render settings flip through the friend
	// access (the same members the panel's widgets touch).
	void WorldRendererElement::HandleShortcuts() {
		if (m_input.isKeyPressed(keybind::UI_TOGGLE)) {
			SetUIMode(!m_uiMode);
		}
		// Gamepad Start mirrors UI toggle so controller-only users
		// can reach the debug panel.
		if (m_input.hasGamepad() &&
			m_input.gamepadButtonPressed(0, SDL_GAMEPAD_BUTTON_START)) {
			SetUIMode(!m_uiMode);
		}
		if (m_input.isKeyPressed(keybind::MOTION_BLUR_TOGGLE)) {
			m_renderer.m_motionBlurEnabled = !m_renderer.m_motionBlurEnabled;
		}
		if (m_input.isKeyPressed(keybind::AO_CYCLE)) {
			// AoMode is a nested WorldRenderer enum — qualify (unlike the
			// namespace-scope AaMode/ViewMode/DsDebugViz).
			m_renderer.m_aoMode = static_cast<WorldRenderer::AoMode>(
				(static_cast<int>(m_renderer.m_aoMode) + 1) % 3); // None -> Ssao -> Sao
		}
		if (m_input.isKeyPressed(keybind::BLOOM_TOGGLE)) {
			m_renderer.m_bloomEnabled = !m_renderer.m_bloomEnabled;
		}
		if (m_input.isKeyPressed(keybind::AUTO_EXPOSURE_TOGGLE)) {
			m_renderer.m_autoExposureEnabled = !m_renderer.m_autoExposureEnabled;
		}
		if (m_input.isKeyPressed(keybind::MOTION_DEBUG_TOGGLE)) {
			m_renderer.m_showMotionDebug = !m_renderer.m_showMotionDebug;
		}
		if (m_input.isKeyPressed(keybind::LOD_TOGGLE)) {
			m_renderer.m_lodEnabled = !m_renderer.m_lodEnabled;
		}
		// F toggles the GPU particle system (visibility only; the pool
		// keeps its state so re-enabling resumes mid-flight).
		if (m_input.isKeyPressed(keybind::PARTICLES_TOGGLE)) {
			m_renderer.m_particlesEnabled = !m_renderer.m_particlesEnabled;
		}
		// J spawns a blood splat on whatever the camera crosshair hits
		// (CPU raycast over the scene triangle soup; the pick BVH builds
		// lazily on first use â€” see SpawnDecalAtCrosshair).
		if (m_input.isKeyPressed(keybind::DECAL_SPLAT)) {
			m_renderer.SpawnDecalAtCrosshair();
		}
		if (m_input.isKeyPressed(keybind::DEFERRED_SHADOWS_TOGGLE)) {
			m_renderer.m_dsEnabled = !m_renderer.m_dsEnabled;
		}
		if (m_input.isKeyPressed(keybind::GFX_FILTER_TOGGLE)) {
			m_renderer.m_useGfxFilter = !m_renderer.m_useGfxFilter;
		}
		// Backtick pops the texture inspector OUT of the debug panel: a
		// separate floating window (see the draw site after the panel End).
		// Closing drops the preview binding + selection (no dead-view
		// sampling, no leaked overlay descriptor).
		if (m_input.isKeyPressed(keybind::TEXTURE_VIEWER_TOGGLE)) {
			m_renderer.m_texDebugWindow = !m_renderer.m_texDebugWindow;
			if (!m_renderer.m_texDebugWindow)
				m_renderer.DropTexDebugSelection();
		}
		if (m_input.isKeyPressed(keybind::WORLD_MAP_TOGGLE)) {
			m_renderer.m_worldMapWindow = !m_renderer.m_worldMapWindow;
		}
		// Ctrl+M pops the VRAM usage viewer: the GPU memory heaps
		// (sizes + VMA budgets/usage) as another floating tool window.
		// The chord mirrors HUD_TOGGLE (Ctrl+T): bare M stays the
		// motion-blur toggle, bare Ctrl stays camera-descend.
		if ((m_input.isKeyHeld(GLFW_KEY_LEFT_CONTROL)
				|| m_input.isKeyHeld(GLFW_KEY_RIGHT_CONTROL))
			&& m_input.isKeyPressed(keybind::VRAM_VIEWER_TOGGLE)) {
			m_renderer.m_vramWindow = !m_renderer.m_vramWindow;
		}
		// Ambient / GI source: N cycles All -> Lightmaps -> Probes ->
		// SkyIbl -> Off (see AmbientMode â€” single-source isolation for
		// bisection, All is the production state).
		// R cycles the baked-GI debug views (see GI_DEBUG_MODE_COUNT).
		// F8 is an alias for R. Both work in every ambient mode.
		if (m_input.isKeyPressed(keybind::AMBIENT_CYCLE)) {
			m_renderer.CycleAmbientMode();
		}
		if (m_input.isKeyPressed(keybind::GI_DEBUG_CYCLE)
			|| m_input.isKeyPressed(keybind::GI_DEBUG_CYCLE_ALT)) {
			keybind::CycleGiDebug(m_renderer.m_giDebug);
		}
		if (m_input.isKeyPressed(keybind::SKY_TOGGLE)) {
			m_renderer.m_skyEnabled = !m_renderer.m_skyEnabled;
		}
		if (m_input.isKeyPressed(keybind::TERRAIN_TOGGLE)) {
			m_renderer.m_terrainEnabled = !m_renderer.m_terrainEnabled;
		}
		// Ctrl+F freezes culling (holds frustum/eye/HZB, camera stays
		// live); bare F stays the particle toggle (Ctrl+M/Ctrl+T chord
		// precedent — works in FPS and UI modes).
		if ((m_input.isKeyHeld(GLFW_KEY_LEFT_CONTROL)
				|| m_input.isKeyHeld(GLFW_KEY_RIGHT_CONTROL))
			&& m_input.isKeyPressed(keybind::CULLING_FREEZE_TOGGLE)) {
			m_renderer.m_freezeCullingView = !m_renderer.m_freezeCullingView;
		}
		// Z cycles AA: Off -> MSAA 2x -> SMAA 1x -> SMAA T2x. 1..4 jump
		// directly to a mode.
		if (m_input.isKeyPressed(keybind::AA_CYCLE)) {
			m_renderer.m_aaMode = static_cast<AaMode>(
				(static_cast<int>(m_renderer.m_aaMode) + 1) % 4);
		}
		if (m_input.isKeyPressed(keybind::AA_OFF)) {
			m_renderer.m_aaMode = AaMode::Off;
		}
		if (m_input.isKeyPressed(keybind::AA_MSAA2X)) {
			m_renderer.m_aaMode = AaMode::Msaa2x;
		}
		if (m_input.isKeyPressed(keybind::AA_SMAA1X)) {
			m_renderer.m_aaMode = AaMode::Smaa1x;
		}
		if (m_input.isKeyPressed(keybind::AA_SMAAT2X)) {
			m_renderer.m_aaMode = AaMode::SmaaT2x;
		}
		if (m_input.isKeyPressed(keybind::CAPTURE)) {
			m_renderer.m_captureQueued = true;
		}
		// F1..F6, F9..F11: the debug view modes (the panel's Debug tab
		// documents each). Lit is the full PBR reference.
		if (m_input.isKeyPressed(keybind::DEBUG_VIEW_LIT)) {
			m_renderer.m_debugView = ViewMode::Lit;
		}
		if (m_input.isKeyPressed(keybind::DEBUG_VIEW_UNLIT)) {
			m_renderer.m_debugView = ViewMode::Unlit;
		}
		if (m_input.isKeyPressed(keybind::DEBUG_VIEW_MESHLETS)) {
			m_renderer.m_debugView = ViewMode::Meshlets;
		}
		if (m_input.isKeyPressed(keybind::DEBUG_VIEW_CASCADES)) {
			m_renderer.m_debugView = ViewMode::Cascades;
		}
		if (m_input.isKeyPressed(keybind::DEBUG_VIEW_LODS)) {
			m_renderer.m_debugView = ViewMode::Lods;
		}
		if (m_input.isKeyPressed(keybind::DEBUG_VIEW_VERTEXCOLORS)) {
			m_renderer.m_debugView = ViewMode::VertexColors;
		}
		if (m_input.isKeyPressed(keybind::DEBUG_VIEW_UV0)) {
			m_renderer.m_debugView = ViewMode::Uv0;
		}
		// F10 (reused from the lightmap-UV view): the cubemap-only IBL
		// view - the filtered lit path with the frame's cubemapDebug
		// pinned to 1 (raw probe radiance). Uv1 lost its key here; it
		// stays reachable via the Debug combo + TOURMALINE_DEBUGVIEW=uv1.
		if (m_input.isKeyPressed(keybind::DEBUG_VIEW_CUBEMAP)) {
			m_renderer.m_debugView = ViewMode::Cubemap;
		}
		if (m_input.isKeyPressed(keybind::DEBUG_VIEW_POM)) {
			m_renderer.m_debugView = ViewMode::Pom;
		}
		// P toggles wireframe (PolygonMode::Line, Cull None) over the
		// active debug view — a raster override, not a shading swap, so
		// it stacks with F1..F6.
		if (m_input.isKeyPressed(keybind::WIREFRAME_TOGGLE)) {
			m_renderer.m_wireframe = !m_renderer.m_wireframe;
		}
		// Ctrl+T toggles the text-only stats HUD (top-left overlay). The
		// chord (either Ctrl) avoids every bare-key binding; T alone does
		// nothing. Works in FPS and UI modes like the other toggles.
		if ((m_input.isKeyHeld(GLFW_KEY_LEFT_CONTROL)
				|| m_input.isKeyHeld(GLFW_KEY_RIGHT_CONTROL))
			&& m_input.isKeyPressed(keybind::HUD_TOGGLE)) {
			m_renderer.m_showHud = !m_renderer.m_showHud;
			std::fprintf(stderr, "[ui] stats HUD %s\n",
				m_renderer.m_showHud ? "on" : "off");
		}
		// F12: live frame-time histogram â€” a separate bottom strip UI
		// (profiler::DrawFrameHistogramUi; the editor's View menu toggles
		// the same state). Zero cost while disabled: RecordFrameTime is a
		// single compare-and-return (see profiler::RecordFrameTime).
		if (m_input.isKeyPressed(keybind::FRAME_HISTOGRAM_TOGGLE)) {
			const bool on = !smvk::profiler::FrameHistogramEnabled();
			smvk::profiler::SetFrameHistogramEnabled(on);
			std::fprintf(stderr, "[profiler] frame-time histogram %s\n",
				on ? "on" : "off");
		}
#if defined(DS_DEBUG_VIZ)
		// Deferred-shadow visualizer cycle (Y): Off -> counts -> diff ->
		// light-scoped modes. Needs the pipeline on (H); see the panel's
		// "DS visualizers" block for the mode list. Y is the key (not F6):
		// F6 selects the vertex-color debug view, and some
		// keyboards/overlays remap or swallow function keys, never
		// letters.
		if (m_input.isKeyPressed(keybind::DS_VIZ_CYCLE)) {
			m_renderer.m_dsDebugMode = static_cast<DsDebugViz>(
				(static_cast<int>(m_renderer.m_dsDebugMode) + 1)
				% (static_cast<int>(DsDebugViz::LightVis) + 1));
		}
#endif
	}

	// FPS camera drive (the old WorldRenderer::UpdateCamera verbatim: the
	// movement keys/gamepad into the renderer's positioner, SDL relative
	// deltas into the virtual cursor, mouselook).
	void WorldRendererElement::UpdateCamera(float dt)
	{
		auto& positioner = m_renderer.CameraPositioner();
		auto& movement = positioner.movement_;

		// Movement only here: toggles live in HandleShortcuts (which owns
		// the centralized keybind:: names, including the R/F8 GI-debug and
		// N ambient cycles - duplicating them here double-fired in FPS
		// mode). The editor drives its own keys - its viewport
		// instance never sees key events, so this stays App-only.
		movement.forward_ = m_input.isKeyHeld(GLFW_KEY_W);
		movement.backward_ = m_input.isKeyHeld(GLFW_KEY_S);
		movement.left_ = m_input.isKeyHeld(GLFW_KEY_A);
		movement.right_ = m_input.isKeyHeld(GLFW_KEY_D);

		// Up and down in camera orientation, not world space.
		movement.up_ = m_input.isKeyHeld(GLFW_KEY_E) || m_input.isKeyHeld(GLFW_KEY_SPACE);
		movement.down_ = m_input.isKeyHeld(GLFW_KEY_Q) || m_input.isKeyHeld(GLFW_KEY_LEFT_CONTROL);

		movement.fastSpeed_ =
			m_input.isKeyHeld(GLFW_KEY_LEFT_SHIFT) ||
			m_input.isKeyHeld(GLFW_KEY_RIGHT_SHIFT);

		// Gamepad camera: left stick moves (camera-relative), right stick
		// looks, triggers rise/sink, right shoulder sprints. Mouse/keyboard
		// above keep working - inputs sum. Sticks use radial deadzones with
		// rescaling (see Input deadzone sliders in Stats).
		if (m_input.hasGamepad()) {
			float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
			m_input.gamepadStick(0, true, lx, ly);
			m_input.gamepadStick(0, false, rx, ry);
			movement.stickMove_.x = lx;
			movement.stickMove_.y = -ly;
			movement.stickLook_.x = rx;
			movement.stickLook_.y = -ry;
			movement.vertical_ =
				m_input.gamepadTrigger(0, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) -
				m_input.gamepadTrigger(0, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
			movement.fastSpeed_ = movement.fastSpeed_ ||
				m_input.gamepadButtonHeld(0, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
		}
		else {
			movement.stickMove_ = glm::vec2(0.0f);
			movement.stickLook_ = glm::vec2(0.0f);
			movement.vertical_ = 0.0f;
		}

		// ---- Mouselook (SDL relative deltas -> virtual cursor) ----
		// SDL relative mode warps the OS cursor every frame, so the absolute
		// x/y positions the old GLFW path consumed are garbage while captured
		// (they ping-pong around the warp point) - that is what broke mouse
		// look. xrel/yrel are valid in BOTH captured and normal modes, so
		// fold them into an unbounded virtual NDC cursor (y down - same
		// mapping the old getMouseX/getMouseY path produced, so
		// rotationSpeed_ keeps its calibration). The cursor is captured in
		// FPS mode (nothing to click), so look is always active here; UI
		// mode never reaches UpdateCamera.
		{
			const float w = static_cast<float>(std::max(1,
				m_renderer.m_swapchain->GetWindowWidth()));
			const float h = static_cast<float>(std::max(1,
				m_renderer.m_swapchain->GetWindowHeight()));
			m_virtualMouse.x += (m_input.getRelativeDeltaX() / w) * 2.0f;
			// Y flip: the old absolute mapping was 1 - (y/h)*2 (screen-up = +NDC),
			// so mouse-down must DECREASE virtual Y for the positioner's
			// pitch = -rotSpeed * deltaY to look down. relY is +down in SDL.
			m_virtualMouse.y -= (m_input.getRelativeDeltaY() / h) * 2.0f;
		}
		const glm::vec2 mousePos = m_virtualMouse;

		if (m_input.isMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT))
		{
			// Zero the pending delta so a click never yanks the camera.
			positioner.resetMousePosition(mousePos);
		}

		positioner.update(
			static_cast<double>(dt),
			mousePos,
			true
		);
	}

	void WorldRendererElement::SetUIMode(bool ui)
	{
		m_uiMode = ui;
		// SDL3: relative mode captures the mouse for FPS; normal mode frees it
		// for ImGui interaction (replaces glfwSetInputMode CURSOR_DISABLED/NORMAL).
		m_host.SetRelativeMouseMode(!ui);
		if (ui) {
			// Center cursor for UI interaction; let ImGui take over.
			// Don't reset camera mouse pos on mode switch to avoid jump when returning to FPS.
		}
		else {
			// When returning to FPS, sync positioner to the virtual cursor so
			// the first relative delta doesn't yank the camera.
			m_renderer.CameraPositioner().resetMousePosition(m_virtualMouse);
		}
		// The renderer's panel footer reads the mode (display only).
		m_renderer.SetUiModeHint(m_uiMode);
	}

} // namespace engine
