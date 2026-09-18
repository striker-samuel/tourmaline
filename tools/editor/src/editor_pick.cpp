// CPU ray picking: mouse -> world ray -> closest mesh node.
//
// Mouse/extent are VIEWPORT-LOCAL (the central rect between the panels;
// the projection is built for that aspect). Exact triangle tests use the
// kept tinygltf model (import is lossless for positions: importer only
// applies the coord-fix rigid transform, mirrored here). Scenes loaded
// from pak (no glTF kept) fall back to AABB picking.

#include "editor_app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/renderview.h>

#include <tiny_gltf.h>

namespace editor {
namespace {

// coordFix 0 (engine default): (-x, y, -z). Mirror of readVec3ForientationFix.
glm::vec3 FixPos(const double* p) {
	return glm::vec3(float(-p[0]), float(p[1]), float(-p[2]));
}

bool ReadPosition(const tinygltf::Model& model, int accessorIdx, size_t i,
	glm::vec3& out) {
	if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size())
		return false;
	const tinygltf::Accessor& acc = model.accessors[accessorIdx];
	if (i >= (size_t)acc.count)
		return false;
	const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
	const tinygltf::Buffer& buf = model.buffers[bv.buffer];
	const size_t stride = acc.ByteStride(bv);
	const size_t off = bv.byteOffset + acc.byteOffset + i * stride;
	if (off + 12 > buf.data.size())
		return false;
	const float* f = reinterpret_cast<const float*>(buf.data.data() + off);
	if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
		return false;
	const double d[3] = { f[0], f[1], f[2] };
	out = FixPos(d);
	return true;
}

bool ReadIndex(const tinygltf::Model& model, int accessorIdx, size_t i,
	uint32_t& out) {
	if (accessorIdx < 0)
		return false;
	const tinygltf::Accessor& acc = model.accessors[accessorIdx];
	if (i >= (size_t)acc.count)
		return false;
	const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
	const tinygltf::Buffer& buf = model.buffers[bv.buffer];
	const size_t stride = acc.ByteStride(bv);
	const size_t off = bv.byteOffset + acc.byteOffset + i * stride;
	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
		if (off + 4 > buf.data.size())
			return false;
		out = *reinterpret_cast<const uint32_t*>(buf.data.data() + off);
		return true;
	}
	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
		if (off + 2 > buf.data.size())
			return false;
		out = *reinterpret_cast<const uint16_t*>(buf.data.data() + off);
		return true;
	}
	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
		if (off + 1 > buf.data.size())
			return false;
		out = buf.data[off];
		return true;
	}
	return false;
}

// Moller-Trumbore. Returns t or -1.
float RayTri(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a,
	const glm::vec3& b, const glm::vec3& c) {
	const glm::vec3 e1 = b - a, e2 = c - a;
	const glm::vec3 p = glm::cross(d, e2);
	const float det = glm::dot(e1, p);
	if (std::fabs(det) < 1e-12f)
		return -1.0f;
	const float inv = 1.0f / det;
	const glm::vec3 t = o - a;
	const float u = glm::dot(t, p) * inv;
	if (u < 0.0f || u > 1.0f)
		return -1.0f;
	const glm::vec3 q = glm::cross(t, e1);
	const float v = glm::dot(d, q) * inv;
	if (v < 0.0f || u + v > 1.0f)
		return -1.0f;
	const float tt = glm::dot(e2, q) * inv;
	return (tt > 1e-6f) ? tt : -1.0f;
}

bool RayAabb(const glm::vec3& o, const glm::vec3& inv, const glm::vec3& mn,
	const glm::vec3& mx, float& tOut) {
	float tmin = 0.0f, tmax = FLT_MAX;
	for (int i = 0; i < 3; ++i) {
		float t0 = (mn[i] - o[i]) * inv[i];
		float t1 = (mx[i] - o[i]) * inv[i];
		if (inv[i] < 0.0f)
			std::swap(t0, t1);
		tmin = std::max(tmin, t0);
		tmax = std::min(tmax, t1);
		if (tmax < tmin)
			return false;
	}
	tOut = tmin;
	return true;
}

} // namespace

glm::vec3 EditorApp::MouseRay(float mouseX, float mouseY, smvk::Extent2D extent,
	const glm::mat4& viewProj, glm::vec3& outOrigin) const {
	// Mouse pixels (top-left origin) -> NDC (Y-down after the Vulkan flip)
	// -> world ray via the inverse view-projection. Depth is REVERSED-Z:
	// NDC z=1 is the near plane, z=0 the far plane (see
	// MakeReversedZProjRH) — unprojecting z=0 as "near" builds a
	// backwards ray that hits geometry behind the camera instead of
	// what's under the cursor.
	const float nx = (extent.width > 0) ? (mouseX / float(extent.width)) * 2.0f - 1.0f : 0.0f;
	const float ny = (extent.height > 0) ? (mouseY / float(extent.height)) * 2.0f - 1.0f : 0.0f;
	const glm::vec4 near = glm::inverse(viewProj) * glm::vec4(nx, ny, 1.0f, 1.0f);
	const glm::vec4 far = glm::inverse(viewProj) * glm::vec4(nx, ny, 0.0f, 1.0f);
	const glm::vec3 n = glm::vec3(near) / near.w;
	const glm::vec3 f = glm::vec3(far) / far.w;
	outOrigin = n;
	return glm::normalize(f - n);
}

std::optional<EditorApp::PickHit> EditorApp::PickNode(int vpIdx, float mouseX,
	float mouseY, smvk::Extent2D extent) const {
	if (!m_world || m_world->scene.hierarchy.empty())
		return std::nullopt;
	glm::vec3 origin;
	const glm::mat4 viewProj = m_viewports[std::clamp(vpIdx, 0, kMaxViewports - 1)].viewProj;
	const glm::vec3 dir = MouseRay(mouseX, mouseY, extent, viewProj, origin);
	const glm::vec3 inv(1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z);

	PickHit best{};
	best.t = FLT_MAX;
	bool found = false;
	const smvk::IteratorFlags req = smvk::IteratorFlags(
		smvk::IteratorFlagBit_Renderable | smvk::IteratorFlagBit_StaticVertexFmt);
	m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
		if (!IsVisibleNode(m_world->scene, int(item.node)))
			return; // hidden subtrees are not pickable
		smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh);
		if (!mesh)
			return;
		const glm::mat4& model = m_world->scene.globalTransform[item.node];
		glm::vec3 c;
		float r = 0.0f;
		smvk::MeshBoundsToWorld(model, mesh->geometry.aabbMin,
			mesh->geometry.aabbMax, c, r);
		// Broadphase: sphere test.
		const glm::vec3 oc = c - origin;
		const float tca = glm::dot(oc, dir);
		if (tca < 0.0f && glm::dot(oc, oc) > r * r)
			return;
		const float d2 = glm::dot(oc, oc) - tca * tca;
		if (d2 > r * r)
			return;
		if (found && tca - r >= best.t)
			return; // cannot beat the current best

		float tHit = FLT_MAX;
		auto it = m_primRef.find(item.node);
		if (m_hasGltfModel && it != m_primRef.end() && it->second.mesh >= 0) {
			// Exact triangle walk over the glTF primitive.
			const tinygltf::Mesh& gm = m_gltfModel.meshes[it->second.mesh];
			if (it->second.prim < (int)gm.primitives.size()) {
				const tinygltf::Primitive& prim = gm.primitives[it->second.prim];
				auto pa = prim.attributes.find("POSITION");
				if (pa != prim.attributes.end()) {
					const size_t vcount =
						m_gltfModel.accessors[pa->second].count;
					glm::vec3 a, b, cc;
					auto vAt = [&](uint32_t idx, glm::vec3& out) {
						glm::vec3 local;
						if (!ReadPosition(m_gltfModel, pa->second, idx, local))
							return false;
						out = glm::vec3(model * glm::vec4(local, 1.0f));
						return true;
					};
					if (prim.indices >= 0) {
						const size_t tcount =
							m_gltfModel.accessors[prim.indices].count / 3;
						for (size_t t = 0; t < tcount; ++t) {
							uint32_t ix[3] = {};
							bool ok = true;
							for (int k = 0; k < 3; ++k)
								ok &= ReadIndex(m_gltfModel, prim.indices,
									t * 3 + k, ix[k]);
							if (!ok || ix[0] >= vcount || ix[1] >= vcount ||
								ix[2] >= vcount)
								continue;
							if (!vAt(ix[0], a) || !vAt(ix[1], b) || !vAt(ix[2], cc))
								continue;
							const float th = RayTri(origin, dir, a, b, cc);
							if (th > 0.0f && th < tHit)
								tHit = th;
						}
					}
					else {
						for (size_t t = 0; t + 2 < vcount; t += 3) {
							if (!vAt(uint32_t(t), a) || !vAt(uint32_t(t + 1), b) ||
								!vAt(uint32_t(t + 2), cc))
								continue;
							const float th = RayTri(origin, dir, a, b, cc);
							if (th > 0.0f && th < tHit)
								tHit = th;
						}
					}
				}
			}
		}
		else {
			// AABB fallback (pak-loaded scenes).
			glm::vec3 mn = glm::vec3(model * glm::vec4(mesh->geometry.aabbMin, 1.0f));
			glm::vec3 mx = glm::vec3(model * glm::vec4(mesh->geometry.aabbMax, 1.0f));
			const glm::vec3 lo = glm::min(mn, mx), hi = glm::max(mn, mx);
			float ta = 0.0f;
			if (RayAabb(origin, inv, lo, hi, ta))
				tHit = ta;
		}
		if (tHit < FLT_MAX && (!found || tHit < best.t)) {
			best.node = item.node;
			best.t = tHit;
			found = true;
		}
		});
	if (!found)
		return std::nullopt;
	return best;
}

} // namespace editor
