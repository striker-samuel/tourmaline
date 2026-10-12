#pragma once
// WorldRendererElement: the CONTROL layer over WorldRenderer.
//
// The split (the "control leaking into the renderer" fix): WorldRenderer is
// the pure frame builder (rendergraph, scene passes, post chain, its own
// settings/debug panel — driven by data + explicit setters); THIS class is
// the IAppElement the host stacks — it owns every control concern:
//   - the INPUT (the host's Input&) and all keybind polling
//     (HandleShortcuts, the old WorldRenderer::HandleShortcuts verbatim),
//   - the camera drive (UpdateCamera + the FPS virtual cursor +
//     SetUIMode's relative-mouse management),
//   - the UI mode (FPS <-> UI) and the camera-control freeze
//     (--disable_control) + bench flight (--bench_move),
//   - the host lifecycle (onAttach/onDetach/onResize forwarded to the
//     renderer's pair; onUpdate runs the control pass before the frame).
//
// The renderer is a member driven through its public API (Renderer() —
// hosts configure it directly: scene id, world source, pass config,
// streaming, DLSS, exposure pins) plus friend access for the shortcuts
// (they flip the same render settings the panel's widgets touch — a
// parallel toggle-method per checkbox would just mirror the panel; the
// control FLOW, who decides and when, is entirely this element's).
//
// The renderer's settings/debug panel stays in WorldRenderer (nvpro-style:
// a renderer may own its settings widgets); its gamepad config + footer
// read the Input*/ui-mode hint this element pushes per frame
// (SetInputForUi/SetUiModeHint) — status display, never control.

#include <Engine/App/Element.h>
#include <Engine/App/input.h>
#include <Engine/App/keybindings.h>
#include <Engine/Camera/camera.h>
#include <Engine/Renderer/WorldRenderer.h>

#include <glm/glm.hpp>

namespace engine {

class WorldRendererElement : public IAppElement {
public:
	explicit WorldRendererElement(smvk::App& host);
	const char* ElementName() const override { return m_renderer.RendererName(); }

	// IAppElement lifecycle: the renderer pair + the control pass.
	bool onAttach() override;
	void onDetach() override;
	void onResize(smvk::Extent2D swapExtent) override;
	void onUpdate(float dt) override;
	void onRender(ElementFrame& frame) override;

	// The wrapped renderer: hosts drive its public API directly (SetSceneId,
	// SetWorldSource, Configure, GetTextureStreamer, SetDlssMode, exposure
	// pins, ...). Control surface stays HERE (SetDisableControl /
	// SetBenchMoveSpeed below).
	WorldRenderer& Renderer() { return m_renderer; }

	// ---- Control surface (moved from WorldRenderer) ------------------
	// Bench freeze (--disable_control): skip every camera-control update
	// (keyboard/gamepad/mouselook) so the camera stays at the scene's
	// configured pose for the whole run and A/B comparisons are
	// deterministic. The I-key UI mode and the feature shortcuts
	// (L/G/H/...) keep working.
	void SetDisableControl(bool v) { m_disableControl = v; }
	// Bench flight (--bench_move <m/s>): integrate the camera straight
	// along the spawn forward vector at a constant velocity past warmup
	// (stationary through warmup so every run starts at the same pose).
	// Deterministic — no input ramp/damping — and drives LOD promotion
	// + texture streaming churn through the measured frames. 0 = off.
	void SetBenchMoveSpeed(float v) { m_benchMoveSpeed = v; }
	// FPS <-> UI mode (the I key / gamepad Start): relative-mouse capture
	// for FPS, free cursor for ImGui (the old WorldRenderer::SetUIMode).
	void SetUIMode(bool ui);

private:
	// Per-frame control pass (the old WorldRenderer::Update): shortcuts,
	// headless decal autosplat, camera drive, bench flight.
	void Update(float dt);
	// Global feature shortcuts (the old WorldRenderer::HandleShortcuts
	// verbatim — see keybindings.h; WASDQE/Space/Shift/Ctrl are reserved
	// for the camera and never used here).
	void HandleShortcuts();
	// FPS camera drive (the old WorldRenderer::UpdateCamera verbatim:
	// keys/gamepad into the positioner, relative deltas into the virtual
	// cursor, mouselook).
	void UpdateCamera(float dt);

	smvk::App& m_host;
	Input& m_input;
	WorldRenderer m_renderer;
	// Control state (moved from the renderer):
	bool m_uiMode = false;
	bool m_disableControl = false;
	float m_benchMoveSpeed = 0.0f;
	// Virtual cursor for FPS mouselook: SDL relative mode warps the real
	// cursor, so absolute positions are garbage while captured. The look
	// deltas (xrel/yrel, valid in BOTH relative and normal mode) fold
	// into this unbounded NDC-space position that feeds the positioner.
	glm::vec2 m_virtualMouse{ 0.0f };
};

} // namespace engine
