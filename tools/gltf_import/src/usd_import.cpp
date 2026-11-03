// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// OpenUSD is an AUTHORING input only. The composed stage is converted to
// tinygltf's in-memory interchange shape, then the existing Tourmaline
// DeriveGltfAssets path writes .geometry/.material/.meta. No USD types or
// libraries appear in the engine, the cooked pak, or the runtime.
//
// Deliberately fail closed on USD constructs this adapter cannot represent.
// Silent loss of a prim is worse than a failed content build.

#include <gltf_import/gltf.h>

#include <pxr/pxr.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool Fail(const std::string& message) {
    std::cerr << "USD import: " << message << '\n';
    return false;
}

struct BufferBuilder {
    tinygltf::Model& model;
    std::vector<unsigned char> bytes;

    int Accessor(const void* data, size_t size, int component,
        int type, size_t count, int target) {
        while (bytes.size() % 4) bytes.push_back(0);
        const size_t offset = bytes.size();
        const auto* p = static_cast<const unsigned char*>(data);
        bytes.insert(bytes.end(), p, p + size);
        tinygltf::BufferView view{};
        view.buffer = 0;
        view.byteOffset = offset;
        view.byteLength = size;
        view.target = target;
        const int viewIndex = static_cast<int>(model.bufferViews.size());
        model.bufferViews.push_back(view);
        tinygltf::Accessor accessor{};
        accessor.bufferView = viewIndex;
        accessor.componentType = component;
        accessor.type = type;
        accessor.count = count;
        const int result = static_cast<int>(model.accessors.size());
        model.accessors.push_back(accessor);
        return result;
    }
    void Finish() {
        tinygltf::Buffer buffer{};
        buffer.name = "usd-composed-geometry";
        buffer.data = std::move(bytes);
        model.buffers.push_back(std::move(buffer));
    }
};

size_t PrimvarIndex(const TfToken& interp, size_t face,
    size_t vertex, size_t corner) {
    if (interp == UsdGeomTokens->constant) return 0;
    if (interp == UsdGeomTokens->uniform) return face;
    if (interp == UsdGeomTokens->faceVarying) return corner;
    return vertex; // vertex / varying
}

struct Converter {
    tinygltf::Model& model;
    BufferBuilder blob;
    std::map<std::string, int> materialIndices;

    explicit Converter(tinygltf::Model& m) : model(m), blob{m, {}} {}

    // Only scalar UsdPreviewSurface inputs are representable for now.
    // Connected shader networks need an explicit texture/MDL translator;
    // don't silently drop them or pretend they became PBR values.
    bool ReadMaterial(const UsdShadeMaterial& material, int& out) {
        if (!material) { out = -1; return true; }
        const std::string key = material.GetPath().GetString();
        const auto existing = materialIndices.find(key);
        if (existing != materialIndices.end()) {
            out = existing->second;
            return true;
        }
        tinygltf::Material m{};
        m.name = key; // unique, stable across traversal/order changes
        bool previewFound = false;
        for (const UsdPrim& child : material.GetPrim().GetChildren()) {
            const UsdShadeShader shader(child);
            if (!shader) continue;
            TfToken id;
            if (!shader.GetIdAttr().Get(&id) || id != TfToken("UsdPreviewSurface"))
                continue;
            if (previewFound)
                return Fail("multiple UsdPreviewSurface shaders: " + key);
            previewFound = true;
            for (const auto& name : {"diffuseColor", "roughness", "metallic",
                                      "opacity", "emissiveColor"}) {
                const UsdShadeInput input = shader.GetInput(TfToken(name));
                if (!input) continue;
                if (input.HasConnectedSource())
                    return Fail("connected material input '" + std::string(name)
                        + "' is not yet supported: " + key);
            }
            GfVec3f color(1.f);
            if (shader.GetInput(TfToken("diffuseColor")))
                shader.GetInput(TfToken("diffuseColor")).Get(&color);
            m.pbrMetallicRoughness.baseColorFactor = {
                color[0], color[1], color[2], 1.0};
            float f = 0.f;
            if (auto input = shader.GetInput(TfToken("metallic")); input && input.Get(&f))
                m.pbrMetallicRoughness.metallicFactor = std::clamp(double(f), 0., 1.);
            if (auto input = shader.GetInput(TfToken("roughness")); input && input.Get(&f))
                m.pbrMetallicRoughness.roughnessFactor = std::clamp(double(f), 0., 1.);
            if (auto input = shader.GetInput(TfToken("opacity")); input && input.Get(&f)) {
                m.pbrMetallicRoughness.baseColorFactor[3] = std::clamp(double(f), 0., 1.);
                if (f < 1.f) m.alphaMode = "BLEND";
            }
            GfVec3f emission(0.f);
            if (auto input = shader.GetInput(TfToken("emissiveColor")); input && input.Get(&emission))
                m.emissiveFactor = {emission[0], emission[1], emission[2]};
        }
        if (!previewFound)
            return Fail("material has no direct UsdPreviewSurface: " + key);
        out = static_cast<int>(model.materials.size());
        materialIndices.emplace(key, out);
        model.materials.push_back(std::move(m));
        return true;
    }

    bool ReadMesh(const UsdPrim& prim, int& outMesh) {
        const UsdGeomMesh mesh(prim);
        VtVec3fArray points;
        VtIntArray counts, vertices;
        if (!mesh.GetPointsAttr().Get(&points)
            || !mesh.GetFaceVertexCountsAttr().Get(&counts)
            || !mesh.GetFaceVertexIndicesAttr().Get(&vertices))
            return Fail("missing mesh topology: " + prim.GetPath().GetString());
        if (points.empty() || counts.empty())
            return Fail("empty mesh: " + prim.GetPath().GetString());

        // Per-face subsets cannot be expressed by this single primitive
        // yet. Refuse rather than applying an incorrect material.
        for (const UsdPrim& child : prim.GetChildren())
            if (child.GetTypeName() == TfToken("GeomSubset"))
                return Fail("GeomSubset material assignments not yet supported: "
                    + prim.GetPath().GetString());

        VtVec3fArray normals;
        mesh.GetNormalsAttr().Get(&normals);
        const TfToken normalInterp = mesh.GetNormalsInterpolation();
        const UsdGeomPrimvar st = UsdGeomPrimvarsAPI(mesh).GetPrimvar(TfToken("st"));
        VtVec2fArray uv;
        if (st && !st.ComputeFlattened(&uv))
            return Fail("bad st primvar: " + prim.GetPath().GetString());
        const TfToken uvInterp = st ? st.GetInterpolation() : TfToken();

        std::vector<float> pos, nrm, tex;
        std::vector<uint32_t> indices;
        size_t cornerOffset = 0;
        TfToken orientation;
        mesh.GetOrientationAttr().Get(&orientation);
        const bool reverse = orientation == UsdGeomTokens->leftHanded;
        for (size_t f = 0; f < counts.size(); ++f) {
            const int n = counts[f];
            if (n < 3 || cornerOffset + size_t(n) > vertices.size())
                return Fail("invalid polygon: " + prim.GetPath().GetString());
            // Only native triangle faces can be copied losslessly here.
            // Subdivision and polygons require an explicit tessellation pass.
            TfToken scheme;
            mesh.GetSubdivisionSchemeAttr().Get(&scheme);
            if (scheme != UsdGeomTokens->none && !scheme.IsEmpty())
                return Fail("subdivision mesh needs tessellation: "
                    + prim.GetPath().GetString());
            if (n > 3)
                return Fail("non-triangle face needs a robust tessellator: "
                    + prim.GetPath().GetString());
            for (int t = 1; t < n - 1; ++t) {
                const std::array<size_t, 3> cs = reverse
                    ? std::array<size_t, 3>{0, size_t(t+1), size_t(t)}
                    : std::array<size_t, 3>{0, size_t(t), size_t(t+1)};
                std::array<GfVec3f, 3> tri;
                for (size_t k = 0; k < 3; ++k) {
                    const int vi = vertices[cornerOffset + cs[k]];
                    if (vi < 0 || size_t(vi) >= points.size())
                        return Fail("point index out of range: " + prim.GetPath().GetString());
                    tri[k] = points[size_t(vi)];
                }
                const GfVec3f cross = GfCross(tri[1] - tri[0], tri[2] - tri[0]);
                const float length = cross.GetLength();
                const GfVec3f flat = length > 1e-10f
                    ? cross / length : GfVec3f(0, 1, 0);
                for (size_t k = 0; k < 3; ++k) {
                    const size_t corner = cornerOffset + cs[k];
                    const size_t vi = size_t(vertices[corner]);
                    for (int axis = 0; axis < 3; ++axis)
                        pos.push_back(tri[k][axis]);
                    GfVec3f normal = flat;
                    if (!normals.empty()) {
                        const size_t ni = PrimvarIndex(normalInterp, f, vi, corner);
                        if (ni >= normals.size())
                            return Fail("normal index out of range: "
                                + prim.GetPath().GetString());
                        normal = normals[ni];
                    }
                    for (int axis = 0; axis < 3; ++axis)
                        nrm.push_back(normal[axis]);
                    if (!uv.empty()) {
                        const size_t ui = PrimvarIndex(uvInterp, f, vi, corner);
                        if (ui >= uv.size())
                            return Fail("UV index out of range: "
                                + prim.GetPath().GetString());
                        tex.push_back(uv[ui][0]);
                        tex.push_back(1.f - uv[ui][1]); // USD bottom-left -> glTF top-left
                    }
                    indices.push_back(static_cast<uint32_t>(indices.size()));
                }
            }
            cornerOffset += size_t(n);
        }
        if (cornerOffset != vertices.size())
            return Fail("topology corner count mismatch: " + prim.GetPath().GetString());

        tinygltf::Primitive primitive{};
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        primitive.attributes["POSITION"] = blob.Accessor(pos.data(),
            pos.size() * sizeof(float), TINYGLTF_COMPONENT_TYPE_FLOAT,
            TINYGLTF_TYPE_VEC3, pos.size() / 3, TINYGLTF_TARGET_ARRAY_BUFFER);
        primitive.attributes["NORMAL"] = blob.Accessor(nrm.data(),
            nrm.size() * sizeof(float), TINYGLTF_COMPONENT_TYPE_FLOAT,
            TINYGLTF_TYPE_VEC3, nrm.size() / 3, TINYGLTF_TARGET_ARRAY_BUFFER);
        if (!tex.empty())
            primitive.attributes["TEXCOORD_0"] = blob.Accessor(tex.data(),
                tex.size() * sizeof(float), TINYGLTF_COMPONENT_TYPE_FLOAT,
                TINYGLTF_TYPE_VEC2, tex.size() / 2, TINYGLTF_TARGET_ARRAY_BUFFER);
        primitive.indices = blob.Accessor(indices.data(),
            indices.size() * sizeof(uint32_t), TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
            TINYGLTF_TYPE_SCALAR, indices.size(), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
        const UsdShadeMaterial mat =
            UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
        if (!ReadMaterial(mat, primitive.material)) return false;
        tinygltf::Mesh gm{};
        gm.name = prim.GetPath().GetString();
        gm.primitives.push_back(std::move(primitive));
        outMesh = static_cast<int>(model.meshes.size());
        model.meshes.push_back(std::move(gm));
        return true;
    }

    bool Visit(const UsdPrim& prim, int parent) {
        if (!prim.IsActive() || !prim.IsDefined()) return true;
        // Shading definitions are resources, not renderable scene nodes.
        if (prim.IsA<UsdShadeMaterial>() || prim.IsA<UsdShadeShader>())
            return true;
        if (prim.IsInstance())
            return Fail("instance proxy expansion not yet supported: "
                + prim.GetPath().GetString());
        if (prim.GetTypeName() == TfToken("PointInstancer"))
            return Fail("PointInstancer is not yet supported: "
                + prim.GetPath().GetString());

        tinygltf::Node node{};
        node.name = prim.GetPath().GetString();
        const UsdGeomXformable xform(prim);
        if (xform) {
            GfMatrix4d local(1.0);
            bool resets = false;
            if (!xform.GetLocalTransformation(&local, &resets))
                return Fail("invalid xform: " + node.name);
            if (resets)
                return Fail("resetXformStack not supported: " + node.name);
            // GfMatrix4d is row-vector, glTF stores column-major column-
            // vector: the 16 memory-order values are the same after transpose.
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    node.matrix.push_back(local[row][col]);
        }
        if (prim.IsA<UsdGeomMesh>()) {
            if (!ReadMesh(prim, node.mesh)) return false;
        } else if (prim.IsA<UsdGeomCamera>()) {
            const UsdGeomCamera c(prim);
            float focal = 0, aperture = 0;
            c.GetFocalLengthAttr().Get(&focal);
            c.GetVerticalApertureAttr().Get(&aperture);
            if (focal <= 0 || aperture <= 0)
                return Fail("invalid camera: " + node.name);
            tinygltf::Camera camera{};
            camera.name = node.name;
            camera.type = "perspective";
            camera.perspective.yfov = 2.0 * std::atan(double(aperture) / (2.0 * focal));
            camera.perspective.znear = 0.01;
            node.camera = static_cast<int>(model.cameras.size());
            model.cameras.push_back(std::move(camera));
        } else if (prim.IsA<UsdGeomScope>() || xform
            || prim.GetTypeName().IsEmpty()) {
            // Scope/Xform and untyped grouping prims preserve hierarchy.
        } else {
            return Fail("unsupported prim schema " + prim.GetTypeName().GetString()
                + ": " + node.name);
        }
        const int id = static_cast<int>(model.nodes.size());
        model.nodes.push_back(std::move(node));
        if (parent >= 0)
            model.nodes[size_t(parent)].children.push_back(id);
        else
            model.scenes.front().nodes.push_back(id);
        for (const UsdPrim& child : prim.GetChildren()) {
            if (!Visit(child, id)) return false;
        }
        return true;
    }

};

} // namespace

bool LoadUsdFileAsModel(tinygltf::Model& model,
    const std::filesystem::path& path, std::vector<std::string>& deps) {
    model = tinygltf::Model{};
    deps.clear();
    const UsdStageRefPtr stage = UsdStage::Open(path.string());
    if (!stage) return Fail("cannot open composed stage: " + path.string());

    // Relative layer dependencies can be copied together into the
    // project's staging dir and included in its content fingerprint.
    const auto root = std::filesystem::absolute(path).parent_path();
    std::set<std::string> layers;
    for (const auto& layer : stage->GetUsedLayers()) {
        if (!layer || layer == stage->GetRootLayer()
            || layer == stage->GetSessionLayer()) continue;
        const std::string real = layer->GetRealPath();
        if (real.empty()) continue; // anonymous/memory session
        std::error_code ec;
        const auto rel = std::filesystem::relative(real, root, ec);
        if (ec || rel.empty() || rel.is_absolute()
            || *rel.begin() == "..")
            return Fail("layer dependency escapes source directory: " + real);
        layers.insert(rel.generic_string());
    }
    deps.assign(layers.begin(), layers.end());

    model.asset.version = "2.0";
    model.asset.generator = "Tourmaline OpenUSD source adapter";
    model.scenes.emplace_back();
    model.scenes.front().name = path.stem().string();
    model.defaultScene = 0;

    Converter cv(model);
    // USD metrics are expressed as meters/unit; glTF/Tourmaline use
    // meters and Y-up. Keep one explicit root conversion node.
    const double scale = UsdGeomGetStageMetersPerUnit(stage);
    if (!std::isfinite(scale) || scale <= 0)
        return Fail("invalid metersPerUnit");
    tinygltf::Node basis{};
    basis.name = "__usd_to_tourmaline_basis";
    if (UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z) {
        basis.matrix = {scale,0,0,0, 0,0,-scale,0,
                        0,scale,0,0, 0,0,0,1};
    } else {
        basis.matrix = {scale,0,0,0, 0,scale,0,0,
                        0,0,scale,0, 0,0,0,1};
    }
    model.nodes.push_back(std::move(basis));
    model.scenes.front().nodes.push_back(0);
    for (const UsdPrim& prim : stage->GetPseudoRoot().GetChildren())
        if (!cv.Visit(prim, 0)) return false;
    if (model.meshes.empty())
        return Fail("composed stage has no supported meshes");
    cv.blob.Finish();
    return true;
}
