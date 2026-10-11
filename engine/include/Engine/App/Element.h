#pragma once
// Render-layer / element system (nvpro_core2 nvapp-style).
//
// The swapchain (acquire / present / resize / present-mode) is owned by the
// HOST frame loop — never by a renderer. A host (PakApp, EditorApp, samples)
// owns the window + Swapchain, pumps the frame, and drives a stack of
// IAppElements. Each element renders into its own sub-rect of the swapchain
// image ("layers"), so several renderers can share one present:
//
//   auto fb = swapchain->AcquireFramebuffer();   // host sync, outside
//   for (auto* el : elements) el->onRender(fb);  // no acquire/present inside
//   swapchain->EndAndPresentFramebuffer(fb);      // host sync, outside
//
// Lifecycle mirrors nvapp::IAppElement:
//   onAttach  — called when added (create pipelines, load resources)
//   onDetach  — called on removal/shutdown (release GPU resources)
//   onResize  — swapchain extent changed (recreate swap-sized targets)
//   onUpdate  — per-frame CPU update (cameras, uniforms, animation)
//   onUIMenu  — menu-bar items (optional, default no-op)
//   onUIRender— ImGui widgets (optional, default no-op)
//   onPreRender — pre-frame setup after acquire (optional, default no-op)
//   onRender  — record GPU commands into fb (NEVER acquire/present)
//
// An element MUST NOT call AcquireFramebuffer / EndAndPresentFramebuffer,
// Recreate, or SetPresentMode. Resize + present-mode changes are the host's
// job; the element observes them through onResize and the per-frame
// Framebuffer (which carries the sync owner's Swapchain*).

#include <Engine/RHI/swapchain.h>

#include <cstdint>
#include <vector>

namespace engine {

// Sub-rect of the swapchain image an element renders into, in pixels.
// Origin is top-left, matching RenderPass::SetOffset. A default-constructed
// rect (full = true) covers the whole swap image.
struct LayerRect {
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t width = 0;   // 0 = full swap width (resolved per frame)
	uint32_t height = 0;  // 0 = full swap height (resolved per frame)
	bool full = true;     // ignore x/y/w/h, cover the whole swap image

	static LayerRect Full() { return LayerRect{}; }
	static LayerRect Rect(uint32_t ox, uint32_t oy, uint32_t w, uint32_t h) {
		LayerRect r{};
		r.x = ox;
		r.y = oy;
		r.width = w;
		r.height = h;
		r.full = false;
		return r;
	}
};

// Per-frame sync + target block handed to elements. Thin view over the
// host's Framebuffer plus the resolved swap extent: elements stay oblivious
// to HOW the frame was acquired (the host always acquires).).
struct ElementFrame {
	smvk::Framebuffer* fb = nullptr; // host-acquired frame (never null in onRender)
	smvk::Extent2D swapExtent{ 0, 0 }; // sync-owner extent (fb->swapchain)
	LayerRect layer{};                 // this element's sub-rect (clamped by host)
};

class IAppElement {
public:
	virtual ~IAppElement() = default;
	virtual const char* ElementName() const = 0;

	// Lifecycle: onAttach runs after the host booted (its graphics members
	// exist); false = attach FAILED (the host's Run aborts loudly). onDetach
	// runs at host shutdown while everything is still alive.
	virtual bool onAttach() { return true; }
	virtual void onDetach() {}
	virtual void onResize(smvk::Extent2D /*swapExtent*/) {}
	virtual void onUpdate(float /*dt*/) {}
	virtual void onUIMenu() {}
	virtual void onUIRender() {}
	virtual void onPreRender(ElementFrame& /*frame*/) {}
	// Record this element's passes into frame.fb. Must not touch swapchain
	// sync (no Acquire / Present / Recreate). Confine swap writes to
	// frame.layer via pass SetOffset/SetExtent; use Load (not Clear) on the
	// swap target when the layer is a strict sub-rect so sibling layers
	// survive. Driver caveat (RTX 5070 Ti, 10/11 device-loss hunt):
	// non-zero render-area origins are suspected behind device losses on
	// this machine (prior art: the sub-rect swap write that wedged
	// nvlddmkm) — prefer full writes at (0,0) into per-layer-sized targets
	// over offset writes into a shared canvas wherever possible.
	virtual void onRender(ElementFrame& frame) = 0;
};

// Ordered stack of elements. The host drives it: update + pre-render +
// render in order (first = bottom layer), UI in order. Removal does not
// detach (the host owns lifetimes); call DetachAll() at shutdown.
class ElementStack {
public:
	void AddElement(IAppElement* el) {
		if (el)
			m_elements.push_back(el);
	}
	void RemoveElement(IAppElement* el) {
		for (auto it = m_elements.begin(); it != m_elements.end(); ++it) {
			if (*it == el) {
				m_elements.erase(it);
				return;
			}
		}
	}
	bool AttachAll() {
		for (auto* el : m_elements)
			if (el && !el->onAttach())
				return false;
		return true;
	}
	void DetachAll() {
		for (auto* el : m_elements)
			if (el)
				el->onDetach();
		m_elements.clear();
	}
	[[nodiscard]] bool Empty() const { return m_elements.empty(); }
	[[nodiscard]] size_t Size() const { return m_elements.size(); }

	void onResize(smvk::Extent2D swapExtent) {
		for (auto* el : m_elements)
			if (el)
				el->onResize(swapExtent);
	}
	void onUpdate(float dt) {
		for (auto* el : m_elements)
			if (el)
				el->onUpdate(dt);
	}
	void onUIMenu() {
		for (auto* el : m_elements)
			if (el)
				el->onUIMenu();
	}
	void onUIRender() {
		for (auto* el : m_elements)
			if (el)
				el->onUIRender();
	}
	void onPreRender(ElementFrame& frame) {
		for (auto* el : m_elements)
			if (el)
				el->onPreRender(frame);
	}
	void onRender(ElementFrame& frame) {
		for (auto* el : m_elements)
			if (el)
				el->onRender(frame);
	}

private:
	std::vector<IAppElement*> m_elements;
};

// Clamp a layer rect into the swap extent. Full layers cover everything;
// sub-rects are clipped (empty intersection = 0x0 at 0,0 — element skips).
inline LayerRect ClampLayerRect(const LayerRect& layer, smvk::Extent2D swap) {
	if (layer.full || (layer.width == 0 && layer.height == 0))
		return LayerRect::Full();
	LayerRect out = layer;
	out.full = false;
	if (out.x >= swap.width || out.y >= swap.height) {
		out.x = 0;
		out.y = 0;
		out.width = 0;
		out.height = 0;
		return out;
	}
	if (out.width == 0 || out.x + out.width > swap.width)
		out.width = swap.width - out.x;
	if (out.height == 0 || out.y + out.height > swap.height)
		out.height = swap.height - out.y;
	return out;
}

} // namespace engine
