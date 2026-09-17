// Selection gizmo: screen-space axis handles drawn as debug lines +
// mouse interaction (translate/rotate/scale). Rotate/scale work in the
// node's LOCAL frame, translate in WORLD axes (noted in the overlay).

#include "editor_app.h"

#include <cfloat>
#include <cmath>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <Engine/Renderer/mesh.h>

namespace editor {
namespace {

struct LineBatch {
	glm::vec4 color{ 1, 1, 1, 1 };
	std::vector<smvk::DebugLineVertex> verts;
};

glm::vec2 ToScreen(const glm::vec3& w, const glm::mat4& vp,
	smvk::Extent2D extent) {
	const glm::vec4 c = vp * glm::vec4(w, 1.0f);
	const float inv = (std::fabs(c.w) > 1e-9f) ? 1.0f / c.w : 0.0f;
	return glm::vec2((c.x * inv * 0.5f + 0.5f) * float(extent.width),
		(c.y * inv * 0.5f + 0.5f) * float(extent.height));
}

float DistPointSeg(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
	const glm::vec2 ab = b - a;
	const float len2 = glm::dot(ab, ab);
	const float t = (len2 > 1e-9f)
		? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
	return glm::length(p - (a + ab * t));
}

void AddCircle(LineBatch& b, const glm::vec3& center, const glm::vec3& u,
	const glm::vec3& v, float r, int segs = 48) {
	for (int i = 0; i < segs; ++i) {
		const float a0 = float(i) / segs * 6.2831853f;
		const float a1 = float(i + 1) / segs * 6.2831853f;
		b.verts.push_back({ center + u * std::cos(a0) * r + v * std::sin(a0) * r });
		b.verts.push_back({ center + u * std::cos(a1) * r + v * std::sin(a1) * r });
	}
}

void AddBox(LineBatch& b, const glm::vec3& mn, const glm::vec3& mx) {
	const glm::vec3 c[8] = {
		{ mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z },
		{ mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z },
		{ mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z },
		{ mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z } };
	static const int e[12][2] = { {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},
		{6,7},{7,4},{0,4},{1,5},{2,6},{3,7} };
	for (auto& k : e) {
		b.verts.push_back({ c[k[0]] });
		b.verts.push_back({ c[k[1]] });
	}
}

// Wire arrowhead: 4-sided pyramid with apex at `tip`, axis `ax`
// (unit), base `len` behind the tip, base half-width `r`.
void AddArrowHead(LineBatch& b, const glm::vec3& tip, const glm::vec3& ax,
	float len, float r) {
	const glm::vec3 bc = tip - ax * len;
	glm::vec3 u = std::fabs(ax.y) < 0.9f ? glm::cross(ax, glm::vec3(0, 1, 0))
		: glm::cross(ax, glm::vec3(1, 0, 0));
	u = glm::normalize(u);
	const glm::vec3 v = glm::cross(ax, u);
	glm::vec3 c[4];
	for (int i = 0; i < 4; ++i) {
		const float a = float(i) / 4 * 6.2831853f + 0.7853982f;
		c[i] = bc + (u * std::cos(a) + v * std::sin(a)) * r;
	}
	for (int i = 0; i < 4; ++i) {
		b.verts.push_back({ tip });
		b.verts.push_back({ c[i] });
		b.verts.push_back({ c[i] });
		b.verts.push_back({ c[(i + 1) % 4] });
	}
}

glm::vec3 GizmoOrigin(const Scene& scene, uint32_t node) {
	return glm::vec3(scene.globalTransform[node][3]);
}

} // namespace

void EditorApp::RecordGizmoPass(smvk::LogicalImageViewHandle swapView,
	smvk::Extent2D extent) {
	if (swapView.empty())
		return;
	if (m_selected == UINT32_MAX || !m_world ||
		m_selected >= m_world->scene.hierarchy.size())
		return;
	if (m_lineProgram.shaderCount != 2)
		return;
	// Build batches: selection AABB (yellow) + gizmo axes.
	std::vector<LineBatch> batches;
	{
		LineBatch box;
		box.color = glm::vec4(1.0f, 0.9f, 0.2f, 1.0f);
		// Union AABB over the selected subtree's mesh nodes (via drawItems).
		bool has = false;
		glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
		std::vector<uint32_t> stack{ m_selected };
		std::unordered_set<uint32_t> inSubtree;
		while (!stack.empty()) {
			const uint32_t n = stack.back();
			stack.pop_back();
			if (n >= m_world->scene.hierarchy.size() || !inSubtree.insert(n).second)
				continue;
			for (int c = m_world->scene.hierarchy[n].firstChild; c != -1;
				c = m_world->scene.hierarchy[c].nextSibling)
				stack.push_back((uint32_t)c);
		}
		for (const auto& item : m_world->drawItems) {
			if (!inSubtree.contains(item.node))
				continue;
			smvk::StaticMesh* m = m_world->GetMesh(item.mesh);
			if (!m)
				continue;
			const glm::mat4& model = m_world->scene.globalTransform[item.node];
			for (int k = 0; k < 8; ++k) {
				const glm::vec3 corner(
					(k & 1) ? m->geometry.aabbMax.x : m->geometry.aabbMin.x,
					(k & 2) ? m->geometry.aabbMax.y : m->geometry.aabbMin.y,
					(k & 4) ? m->geometry.aabbMax.z : m->geometry.aabbMin.z);
				const glm::vec3 w = glm::vec3(model * glm::vec4(corner, 1.0f));
				mn = glm::min(mn, w);
				mx = glm::max(mx, w);
				has = true;
			}
		}
		if (has) {
			AddBox(box, mn, mx);
			batches.push_back(std::move(box));
		}
	}

	const glm::vec3 O = GizmoOrigin(m_world->scene, m_selected);
	const float dist = glm::distance(m_camera.getPosition(), O);
	const float s = std::max(dist * 0.14f, 1e-4f);
	const glm::mat4& global = m_world->scene.globalTransform[m_selected];
	const glm::vec3 axesW[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
	glm::vec3 axesL[3];
	for (int i = 0; i < 3; ++i) {
		axesL[i] = glm::normalize(glm::vec3(global * glm::vec4(axesW[i], 0.0f)));
		if (!std::isfinite(axesL[i].x))
			axesL[i] = axesW[i];
	}
	const glm::vec4 axisColors[3] = {
		{1, 0.25f, 0.25f, 1}, {0.3f, 1, 0.3f, 1}, {0.35f, 0.5f, 1, 1} };
	const bool useLocal = (m_gizmoMode != GizmoMode::Translate);
	for (int i = 0; i < 3; ++i) {
		const glm::vec3 ax = (useLocal ? axesL[i] : axesW[i]);
		LineBatch b;
		b.color = (m_gizmoDragging && m_gizmoAxis == i)
			? glm::vec4(1, 1, 0, 1) : axisColors[i];
		if (m_gizmoMode == GizmoMode::Rotate) {
			const glm::vec3 u = glm::normalize(
				std::fabs(ax.y) < 0.9f ? glm::cross(ax, glm::vec3(0, 1, 0))
				: glm::cross(ax, glm::vec3(1, 0, 0)));
			const glm::vec3 v = glm::cross(ax, u);
			AddCircle(b, O, u, v, s);
		}
		else if (m_gizmoMode == GizmoMode::Translate) {
			// Maya-style arrow: shaft + wire pyramid head at the tip.
			const glm::vec3 tip = O + ax * s;
			b.verts.push_back({ O });
			b.verts.push_back({ tip - ax * s * 0.22f });
			AddArrowHead(b, tip, ax, s * 0.22f, s * 0.06f);
		}
		else {
			// Maya-style scale: shaft + wire cube handle at the tip.
			const glm::vec3 tip = O + ax * s;
			b.verts.push_back({ O });
			b.verts.push_back({ tip });
			const glm::vec3 h(s * 0.055f);
			AddBox(b, tip - h, tip + h);
		}
		batches.push_back(std::move(b));
	}

	// Flatten into one upload with per-batch draw ranges.
	struct Range { uint32_t first, count; glm::vec4 color; };
	std::vector<Range> ranges;
	std::vector<smvk::DebugLineVertex> all;
	for (auto& b : batches) {
		if (b.verts.empty())
			continue;
		ranges.push_back({ uint32_t(all.size()), uint32_t(b.verts.size()), b.color });
		all.insert(all.end(), b.verts.begin(), b.verts.end());
	}
	if (all.empty())
		return;
	const size_t bytes = all.size() * sizeof(all[0]);
	if (m_gizmoLines.empty() || m_gizmoLinesCap < uint32_t(all.size())) {
		m_gizmoLines = m_context->CreateBuffer(bytes,
			smvk::BufferType_Vertex, "editor gizmo lines");
		m_gizmoLinesCap = uint32_t(all.size());
	}
	if (m_gizmoLines.empty())
		return;
	m_context->UploadDataToBuffer(m_gizmoLines, all.data(), bytes,
		smvk::PipelineStageBit_Vertex, smvk::AccessFlags::VertexAttributeRead);

	auto& pass = m_rendergraph->AddPass("EditorGizmo")
		.SetOffset(0, 0)
		.SetExtent(extent)
		.SetColor({ swapView },
			smvk::AttachmentLoadOp::Load, smvk::AttachmentStoreOp::Store,
			smvk::ClearColor(0, 0, 0, 1));
	pass.SetRecordingFunc([this, ranges = std::move(ranges)](smvk::CommandBuffer& cmd) {
		if (m_gizmoLines.empty() || m_lineProgram.shaderCount != 2)
			return;
		cmd.BindRenderPipeline(m_lineProgram,
			smvk::VertexFormat::DebugLine,
			smvk::DepthSettings{ .depthFunc = smvk::CompareOp::Always,
				.writeEnable = false, .depthTestEnable = false },
			smvk::StencilSettings{}, smvk::BlendSettings{},
			smvk::MSAASettings{}, smvk::RasterSettings{ .cullMode = smvk::CullMode::None },
			smvk::PrimitiveTopology::LineList);
		cmd.BindVertexBuffer(m_gizmoLines, 0, 0);
		for (const auto& r : ranges) {
			app::LineDebugPush lp{};
			std::memcpy(lp.viewProj, glm::value_ptr(m_lastViewProj), sizeof(lp.viewProj));
			lp.color[0] = r.color.x;
			lp.color[1] = r.color.y;
			lp.color[2] = r.color.z;
			lp.color[3] = r.color.w;
			cmd.PushConstants(0, sizeof(lp), &lp, smvk::ShaderStage::All);
			cmd.Draw(r.count, 1, r.first, 0);
		}
		});
}

bool EditorApp::GizmoHandleInput(smvk::Extent2D extent) {
	if (m_selected == UINT32_MAX || !m_world ||
		m_selected >= m_world->scene.hierarchy.size())
		return false;
	const glm::vec3 O = GizmoOrigin(m_world->scene, m_selected);
	const float dist = glm::distance(m_camera.getPosition(), O);
	const float s = std::max(dist * 0.14f, 1e-4f);
	const bool useLocal = (m_gizmoMode != GizmoMode::Translate);
	const glm::mat4& global = m_world->scene.globalTransform[m_selected];
	const glm::vec3 axesW[3] = { {1,0,0}, {0,1,0}, {0,0,1} };

	const float mx = float(m_input.getMouseX());
	const float my = float(m_input.getMouseY());

	if (!m_gizmoDragging) {
		// Hit test: nearest axis segment in screen pixels.
		int best = -1;
		float bestD = 10.0f;
		for (int i = 0; i < 3; ++i) {
			glm::vec3 ax = axesW[i];
			if (useLocal) {
				ax = glm::vec3(global * glm::vec4(axesW[i], 0.0f));
				if (glm::length(ax) < 1e-6f)
					continue;
				ax = glm::normalize(ax);
			}
			glm::vec3 end = O + ax * s;
			if (m_gizmoMode == GizmoMode::Rotate) {
				// Rings: accept grabs near the projected circle — sample it.
				const glm::vec3 u = glm::normalize(
					std::fabs(ax.y) < 0.9f ? glm::cross(ax, glm::vec3(0, 1, 0))
					: glm::cross(ax, glm::vec3(1, 0, 0)));
				const glm::vec3 v = glm::cross(ax, u);
				for (int k = 0; k < 24; ++k) {
					const float a0 = float(k) / 24 * 6.2831853f;
					const float a1 = float(k + 1) / 24 * 6.2831853f;
					const glm::vec2 p0 = ToScreen(O + (u * std::cos(a0) + v * std::sin(a0)) * s,
						m_lastViewProj, extent);
					const glm::vec2 p1 = ToScreen(O + (u * std::cos(a1) + v * std::sin(a1)) * s,
						m_lastViewProj, extent);
					const float d = DistPointSeg({ mx, my }, p0, p1);
					if (d < bestD) {
						bestD = d;
						best = i;
					}
				}
				continue;
			}
		const glm::vec2 tipPx = ToScreen(end, m_lastViewProj, extent);
		float d = DistPointSeg({ mx, my },
			ToScreen(O, m_lastViewProj, extent), tipPx);
		// Generous grab on the head (arrow tip / scale cube).
		if (glm::length(glm::vec2(mx, my) - tipPx) < 16.0f)
			d = 0.0f;
			if (d < bestD) {
				bestD = d;
				best = i;
			}
		}
		if (best < 0)
			return false;
		// Begin drag: snapshot the full baseline local matrix + grab data.
		// The drag writes live values; release pushes ONE undoable command.
		m_gizmoDragging = true;
		m_gizmoAxis = best;
		m_dragBaseMat = m_world->scene.localTransform[m_selected];
		ReadLocalTRS(m_selected, m_dragCurT, m_dragCurR, m_dragCurS);
		m_dragBaseT = m_dragCurT;
		m_dragBaseR = m_dragCurR;
		m_dragBaseS = m_dragCurS;
		m_dragGrabPoint = O;
		m_dragNodeOrigin = O;
		m_dragLastX = mx;
		m_dragStartDist = s;
		// Ray-plane for translate/scale: plane through O facing the camera
		// around the axis (axis-plane drag).
		glm::vec3 origin;
		const glm::vec3 ray = MouseRay(mx, my, extent, origin);
		glm::vec3 ax = useLocal
			? glm::normalize(glm::vec3(global * glm::vec4(axesW[best], 0.0f))) : axesW[best];
		const glm::vec3 camDir = glm::normalize(O - m_camera.getPosition());
		m_dragPlaneN = glm::normalize(glm::cross(ax, glm::cross(camDir, ax)));
		const float denom = glm::dot(ray, m_dragPlaneN);
		if (std::fabs(denom) > 1e-9f) {
			const float t = glm::dot(O - origin, m_dragPlaneN) / denom;
			if (t > 0.0f)
				m_dragGrabPoint = origin + ray * t;
		}
		m_dragBaseAngle = 0.0f;
		return true;
	}

	// Continue drag (or finish on release).
	if (!m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT)) {
		m_gizmoDragging = false;
		// One undoable command for the whole gesture.
		EditCmd c{};
		c.node = m_selected;
		c.beforeMat = m_dragBaseMat;
		if (m_gizmoMode == GizmoMode::Translate) {
			c.verb = EditVerb::Translate;
			c.afterVec = m_dragCurT;
		}
		else if (m_gizmoMode == GizmoMode::Rotate) {
			c.verb = EditVerb::Rotate;
			c.afterVec = m_dragCurR;
		}
		else {
			c.verb = EditVerb::Scale;
			c.afterVec = m_dragCurS;
		}
		m_gizmoAxis = -1;
		PushDone(c);
		Log("Gizmo edit (" + std::string(
			m_gizmoMode == GizmoMode::Translate ? "move"
			: m_gizmoMode == GizmoMode::Rotate ? "rotate" : "scale") +
			" #" + std::to_string(m_selected) + ")");
		return true;
	}
	const int axIdx = m_gizmoAxis;
	if (axIdx < 0 || axIdx > 2)
		return true;
	glm::vec3 ax = useLocal
		? glm::vec3(global * glm::vec4(axesW[axIdx], 0.0f)) : axesW[axIdx];
	if (glm::length(ax) < 1e-9f)
		return true;
	ax = glm::normalize(ax);

	if (m_gizmoMode == GizmoMode::Translate) {
		glm::vec3 origin;
		const glm::vec3 ray = MouseRay(mx, my, extent, origin);
		const float denom = glm::dot(ray, m_dragPlaneN);
		if (std::fabs(denom) > 1e-9f) {
			const float t = glm::dot(O - origin, m_dragPlaneN) / denom;
			if (t > 0.0f) {
				const glm::vec3 cur = origin + ray * t;
				const float d = glm::dot(cur - m_dragGrabPoint, ax);
				// World delta -> parent-local delta (rotation part only).
				const int parent = m_world->scene.hierarchy[m_selected].parent;
				glm::vec3 dLocal = ax * d;
				if (parent >= 0) {
					const glm::mat3 pw(m_world->scene.globalTransform[parent]);
					const float det = glm::determinant(pw);
					if (std::fabs(det) > 1e-12f)
						dLocal = glm::inverse(pw) * (ax * d);
				}
				m_dragCurT = m_dragBaseT + dLocal;
				WriteLocalTRS(m_selected, m_dragCurT, m_dragCurR, m_dragCurS);
			}
		}
	}
	else if (m_gizmoMode == GizmoMode::Rotate) {
		const float dx = mx - m_dragLastX;
		m_dragLastX = mx;
		m_dragBaseAngle += dx * 0.008f; // radians per pixel
		m_dragCurR = m_dragBaseR;
		m_dragCurR[axIdx] += glm::degrees(m_dragBaseAngle);
		WriteLocalTRS(m_selected, m_dragCurT, m_dragCurR, m_dragCurS);
	}
	else { // Scale
		glm::vec3 origin;
		const glm::vec3 ray = MouseRay(mx, my, extent, origin);
		const float denom = glm::dot(ray, m_dragPlaneN);
		float ratio = 1.0f;
		if (std::fabs(denom) > 1e-9f) {
			const float t = glm::dot(O - origin, m_dragPlaneN) / denom;
			if (t > 0.0f) {
				const glm::vec3 cur = origin + ray * t;
				const float d0 = glm::dot(m_dragGrabPoint - m_dragNodeOrigin, ax);
				const float d1 = glm::dot(cur - m_dragNodeOrigin, ax);
				if (std::fabs(d0) > 1e-6f)
					ratio = std::clamp(d1 / d0, 0.05f, 50.0f);
				else
					ratio = std::clamp(1.0f + (d1 / m_dragStartDist), 0.05f, 50.0f);
			}
		}
		m_dragCurS = m_dragBaseS;
		m_dragCurS[axIdx] = m_dragBaseS[axIdx] * ratio;
		WriteLocalTRS(m_selected, m_dragCurT, m_dragCurR, m_dragCurS);
	}
	return true;
}

} // namespace editor
