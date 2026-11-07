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

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/subset.h>
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
#include <numeric>
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

    int Accessor(const void* data, size_t size, int component, int type, size_t count, int target) {
        while (bytes.size() % 4)
            bytes.push_back(0);
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

size_t PrimvarIndex(const TfToken& interp, size_t face, size_t vertex, size_t corner) {
    if (interp == UsdGeomTokens->constant)
        return 0;
    if (interp == UsdGeomTokens->uniform)
        return face;
    if (interp == UsdGeomTokens->faceVarying)
        return corner;
    return vertex; // vertex / varying
}

// Ear clipping in the dominant projection preserves per-face-vertex primvars.
// Unlike triangle fans, this handles concave planar USD polygons; ambiguous,
// non-planar, and degenerate faces fail instead of silently corrupting geometry.
bool TriangulateFace(const std::vector<GfVec3f>& polygon,
                     std::vector<std::array<size_t, 3>>& triangles) {
    triangles.clear();
    if (polygon.size() < 3)
        return false;
    GfVec3f normal(0.f);
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % polygon.size()];
        normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
        normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
        normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    const float length = normal.GetLength();
    if (length <= 1e-12f)
        return false;
    normal /= length;
    float span = 0.f;
    for (const auto& point : polygon)
        for (int axis = 0; axis < 3; ++axis)
            span = std::max(span, std::abs(point[axis] - polygon[0][axis]));
    if (span <= 1e-8f)
        return false;
    for (const auto& point : polygon)
        if (std::abs(GfDot(point - polygon[0], normal)) > span * 1e-4f)
            return false;

    int drop = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(normal[i]) > std::abs(normal[drop]))
            drop = i;
    const int u = (drop + 1) % 3;
    const int v = (drop + 2) % 3;
    const auto cross = [&](size_t a, size_t b, size_t c) {
        return (double(polygon[b][u]) - polygon[a][u]) * (double(polygon[c][v]) - polygon[a][v]) -
               (double(polygon[b][v]) - polygon[a][v]) * (double(polygon[c][u]) - polygon[a][u]);
    };
    double area = 0.0;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % polygon.size()];
        area += double(a[u]) * b[v] - double(b[u]) * a[v];
    }
    const double epsilon = double(span) * span * 1e-12;
    if (std::abs(area) <= epsilon)
        return false;
    const double sign = area > 0.0 ? 1.0 : -1.0;
    std::vector<size_t> remaining(polygon.size());
    std::iota(remaining.begin(), remaining.end(), 0);
    while (remaining.size() > 3) {
        bool clipped = false;
        for (size_t i = 0; i < remaining.size(); ++i) {
            const size_t a = remaining[(i + remaining.size() - 1) % remaining.size()];
            const size_t b = remaining[i];
            const size_t c = remaining[(i + 1) % remaining.size()];
            if (sign * cross(a, b, c) <= epsilon)
                continue;
            bool containsVertex = false;
            for (const size_t point : remaining) {
                if (point == a || point == b || point == c)
                    continue;
                if (sign * cross(a, b, point) >= -epsilon &&
                    sign * cross(b, c, point) >= -epsilon &&
                    sign * cross(c, a, point) >= -epsilon) {
                    containsVertex = true;
                    break;
                }
            }
            if (containsVertex)
                continue;
            triangles.push_back({a, b, c});
            remaining.erase(remaining.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped)
            return false;
    }
    if (sign * cross(remaining[0], remaining[1], remaining[2]) <= epsilon)
        return false;
    triangles.push_back({remaining[0], remaining[1], remaining[2]});
    return true;
}

struct Converter {
    tinygltf::Model& model;
    BufferBuilder blob;
    std::map<std::string, int> materialIndices;
    std::set<std::string> stableIds;

    explicit Converter(tinygltf::Model& m) : model(m), blob{m, {}} {}

    // Opt-in identity that survives a USD prim rename/reparent. Names without
    // this authored attribute fall back to the composed prim path. Reject
    // duplicates: silently aliasing distinct objects would break .tour edits.
    bool IdentityName(const UsdPrim& prim, std::string& name) {
        const UsdAttribute idAttr = prim.GetAttribute(TfToken("tourmaline:stableId"));
        if (!idAttr) {
            name = prim.GetPath().GetString();
            return true;
        }
        std::string id;
        if (!idAttr.Get(&id) || id.empty() || id.size() > 128 ||
            !std::all_of(id.begin(), id.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '-' || c == '.' || c == ':';
            }))
            return Fail("invalid tourmaline:stableId on " + prim.GetPath().GetString());
        if (!stableIds.insert(id).second)
            return Fail("duplicate tourmaline:stableId '" + id + "' on " +
                        prim.GetPath().GetString());
        name = "usd-id:" + id;
        return true;
    }

    static std::string SortKey(const UsdPrim& prim) {
        std::string id;
        const UsdAttribute attr = prim.GetAttribute(TfToken("tourmaline:stableId"));
        if (attr && attr.Get(&id) && !id.empty())
            return "0/" + id;
        return "1/" + prim.GetPath().GetString();
    }

    bool VisitChildren(const UsdPrim& parent, int id) {
        std::vector<UsdPrim> children;
        for (const auto& child : parent.GetChildren())
            children.push_back(child);
        // Composed USD child order can be explicitly reordered in DCCs.
        // Deterministic traversal reduces numeric .tour ID churn; authored
        // stable IDs additionally survive renames when the hierarchy holds.
        std::sort(children.begin(), children.end(), [](const UsdPrim& a, const UsdPrim& b) {
            return SortKey(a) < SortKey(b);
        });
        for (const auto& child : children)
            if (!Visit(child, id))
                return false;
        return true;
    }

    // Only scalar UsdPreviewSurface inputs are representable for now.
    // Connected shader networks need an explicit texture/MDL translator;
    // don't silently drop them or pretend they became PBR values.
    bool ReadMaterial(const UsdShadeMaterial& material, int& out) {
        if (!material) {
            out = -1;
            return true;
        }
        const std::string key = material.GetPath().GetString();
        const auto existing = materialIndices.find(key);
        if (existing != materialIndices.end()) {
            out = existing->second;
            return true;
        }
        tinygltf::Material m{};
        if (!IdentityName(material.GetPrim(), m.name))
            return false;
        bool previewFound = false;
        for (const UsdPrim& child : material.GetPrim().GetChildren()) {
            const UsdShadeShader shader(child);
            if (!shader)
                continue;
            TfToken id;
            if (!shader.GetIdAttr().Get(&id) || id != TfToken("UsdPreviewSurface"))
                continue;
            if (previewFound)
                return Fail("multiple UsdPreviewSurface shaders: " + key);
            previewFound = true;
            for (const auto& name :
                 {"diffuseColor", "roughness", "metallic", "opacity", "emissiveColor"}) {
                const UsdShadeInput input = shader.GetInput(TfToken(name));
                if (!input)
                    continue;
                if (input.HasConnectedSource())
                    return Fail("connected material input '" + std::string(name) +
                                "' is not yet supported: " + key);
            }
            GfVec3f color(1.f);
            if (shader.GetInput(TfToken("diffuseColor")))
                shader.GetInput(TfToken("diffuseColor")).Get(&color);
            m.pbrMetallicRoughness.baseColorFactor = {color[0], color[1], color[2], 1.0};
            float f = 0.f;
            if (auto input = shader.GetInput(TfToken("metallic")); input && input.Get(&f))
                m.pbrMetallicRoughness.metallicFactor = std::clamp(double(f), 0., 1.);
            if (auto input = shader.GetInput(TfToken("roughness")); input && input.Get(&f))
                m.pbrMetallicRoughness.roughnessFactor = std::clamp(double(f), 0., 1.);
            if (auto input = shader.GetInput(TfToken("opacity")); input && input.Get(&f)) {
                m.pbrMetallicRoughness.baseColorFactor[3] = std::clamp(double(f), 0., 1.);
                if (f < 1.f)
                    m.alphaMode = "BLEND";
            }
            GfVec3f emission(0.f);
            if (auto input = shader.GetInput(TfToken("emissiveColor"));
                input && input.Get(&emission))
                m.emissiveFactor = {emission[0], emission[1], emission[2]};
        }
        if (!previewFound)
            return Fail("material has no direct UsdPreviewSurface: " + key);
        out = static_cast<int>(model.materials.size());
        materialIndices.emplace(key, out);
        model.materials.push_back(std::move(m));
        return true;
    }

    bool ReadMesh(const UsdPrim& prim, const std::string& stableName, int& outMesh) {
        const UsdGeomMesh mesh(prim);
        VtVec3fArray points;
        VtIntArray counts, vertices;
        if (!mesh.GetPointsAttr().Get(&points) || !mesh.GetFaceVertexCountsAttr().Get(&counts) ||
            !mesh.GetFaceVertexIndicesAttr().Get(&vertices))
            return Fail("missing mesh topology: " + prim.GetPath().GetString());
        if (points.empty() || counts.empty())
            return Fail("empty mesh: " + prim.GetPath().GetString());

        TfToken scheme;
        mesh.GetSubdivisionSchemeAttr().Get(&scheme);
        if (scheme != UsdGeomTokens->none && !scheme.IsEmpty())
            return Fail("subdivision mesh needs tessellation: " + prim.GetPath().GetString());

        // A materialBind GeomSubset assigns whole faces to a material.
        // Overlapping, malformed, or unsupported subsets fail closed.
        std::vector<UsdShadeMaterial> faceMaterials(
            counts.size(), UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial());
        std::vector<bool> assigned(counts.size(), false);
        for (const UsdPrim& child : prim.GetChildren()) {
            if (!child.IsA<UsdGeomSubset>())
                continue;
            const UsdGeomSubset subset(child);
            TfToken element, family;
            subset.GetElementTypeAttr().Get(&element);
            subset.GetFamilyNameAttr().Get(&family);
            if (family != TfToken("materialBind"))
                continue;
            if (element != UsdGeomTokens->face)
                return Fail("materialBind subset must target faces: " +
                            child.GetPath().GetString());
            VtIntArray faces;
            if (!subset.GetIndicesAttr().Get(&faces))
                return Fail("missing subset indices: " + child.GetPath().GetString());
            const UsdShadeMaterial material =
                UsdShadeMaterialBindingAPI(child).ComputeBoundMaterial();
            if (!material)
                return Fail("unbound materialBind subset: " + child.GetPath().GetString());
            for (const int face : faces) {
                if (face < 0 || size_t(face) >= counts.size() || assigned[size_t(face)])
                    return Fail("overlapping/out-of-range material subset: " +
                                child.GetPath().GetString());
                assigned[size_t(face)] = true;
                faceMaterials[size_t(face)] = material;
            }
        }

        VtVec3fArray normals;
        mesh.GetNormalsAttr().Get(&normals);
        const TfToken normalInterp = mesh.GetNormalsInterpolation();
        const UsdGeomPrimvar st = UsdGeomPrimvarsAPI(mesh).GetPrimvar(TfToken("st"));
        VtVec2fArray uv;
        if (st && !st.ComputeFlattened(&uv))
            return Fail("bad st primvar: " + prim.GetPath().GetString());
        const TfToken uvInterp = st ? st.GetInterpolation() : TfToken();

        struct Group {
            UsdShadeMaterial material;
            std::vector<float> pos, nrm, tex;
            std::vector<uint32_t> indices;
        };
        std::map<std::string, Group> groups;
        size_t cornerOffset = 0;
        TfToken orientation;
        mesh.GetOrientationAttr().Get(&orientation);
        const bool reverse = orientation == UsdGeomTokens->leftHanded;
        for (size_t f = 0; f < counts.size(); ++f) {
            const int n = counts[f];
            if (n < 3 || cornerOffset + size_t(n) > vertices.size())
                return Fail("invalid polygon: " + prim.GetPath().GetString());
            std::vector<GfVec3f> polygon;
            polygon.reserve(size_t(n));
            for (int i = 0; i < n; ++i) {
                const int vi = vertices[cornerOffset + size_t(i)];
                if (vi < 0 || size_t(vi) >= points.size())
                    return Fail("point index out of range: " + prim.GetPath().GetString());
                polygon.push_back(points[size_t(vi)]);
            }
            std::vector<std::array<size_t, 3>> triangles;
            if (!TriangulateFace(polygon, triangles))
                return Fail("non-planar, degenerate, or invalid polygon: " +
                            prim.GetPath().GetString());

            const UsdShadeMaterial material = faceMaterials[f];
            const std::string matKey = material ? material.GetPath().GetString() : std::string();
            Group& group = groups[matKey];
            group.material = material;
            for (auto corners : triangles) {
                if (reverse)
                    std::swap(corners[1], corners[2]);
                const GfVec3f cross = GfCross(polygon[corners[1]] - polygon[corners[0]],
                                              polygon[corners[2]] - polygon[corners[0]]);
                const float length = cross.GetLength();
                if (length <= 1e-12f)
                    return Fail("degenerate triangle: " + prim.GetPath().GetString());
                const GfVec3f flat = cross / length;
                for (const size_t c : corners) {
                    const size_t corner = cornerOffset + c;
                    const size_t vi = size_t(vertices[corner]);
                    for (int axis = 0; axis < 3; ++axis)
                        group.pos.push_back(polygon[c][axis]);
                    GfVec3f normal = flat;
                    if (!normals.empty()) {
                        const size_t ni = PrimvarIndex(normalInterp, f, vi, corner);
                        if (ni >= normals.size())
                            return Fail("normal index out of range: " + prim.GetPath().GetString());
                        normal = normals[ni];
                    }
                    for (int axis = 0; axis < 3; ++axis)
                        group.nrm.push_back(normal[axis]);
                    if (!uv.empty()) {
                        const size_t ui = PrimvarIndex(uvInterp, f, vi, corner);
                        if (ui >= uv.size())
                            return Fail("UV index out of range: " + prim.GetPath().GetString());
                        group.tex.push_back(uv[ui][0]);
                        group.tex.push_back(1.f - uv[ui][1]);
                    }
                    group.indices.push_back(static_cast<uint32_t>(group.indices.size()));
                }
            }
            cornerOffset += size_t(n);
        }
        if (cornerOffset != vertices.size())
            return Fail("topology corner count mismatch: " + prim.GetPath().GetString());

        tinygltf::Mesh gm{};
        gm.name = stableName;
        for (auto& [materialName, group] : groups) {
            tinygltf::Primitive primitive{};
            primitive.mode = TINYGLTF_MODE_TRIANGLES;
            primitive.attributes["POSITION"] = blob.Accessor(group.pos.data(),
                                                             group.pos.size() * sizeof(float),
                                                             TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                             TINYGLTF_TYPE_VEC3,
                                                             group.pos.size() / 3,
                                                             TINYGLTF_TARGET_ARRAY_BUFFER);
            primitive.attributes["NORMAL"] = blob.Accessor(group.nrm.data(),
                                                           group.nrm.size() * sizeof(float),
                                                           TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                           TINYGLTF_TYPE_VEC3,
                                                           group.nrm.size() / 3,
                                                           TINYGLTF_TARGET_ARRAY_BUFFER);
            if (!group.tex.empty())
                primitive.attributes["TEXCOORD_0"] = blob.Accessor(group.tex.data(),
                                                                   group.tex.size() * sizeof(float),
                                                                   TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                                   TINYGLTF_TYPE_VEC2,
                                                                   group.tex.size() / 2,
                                                                   TINYGLTF_TARGET_ARRAY_BUFFER);
            primitive.indices = blob.Accessor(group.indices.data(),
                                              group.indices.size() * sizeof(uint32_t),
                                              TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                              TINYGLTF_TYPE_SCALAR,
                                              group.indices.size(),
                                              TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            if (!ReadMaterial(group.material, primitive.material))
                return false;
            gm.primitives.push_back(std::move(primitive));
        }
        outMesh = static_cast<int>(model.meshes.size());
        model.meshes.push_back(std::move(gm));
        return true;
    }

    bool Visit(const UsdPrim& prim, int parent) {
        if (!prim.IsActive() || !prim.IsDefined())
            return true;
        // Shading definitions are resources, not renderable scene nodes.
        if (prim.IsA<UsdShadeMaterial>() || prim.IsA<UsdShadeShader>() || prim.IsA<UsdGeomSubset>())
            return true;
        if (prim.IsInstance())
            return Fail("instance proxy expansion not yet supported: " +
                        prim.GetPath().GetString());
        if (prim.GetTypeName() == TfToken("PointInstancer"))
            return Fail("PointInstancer is not yet supported: " + prim.GetPath().GetString());

        tinygltf::Node node{};
        if (!IdentityName(prim, node.name))
            return false;
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
            if (!ReadMesh(prim, node.name, node.mesh))
                return false;
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
        } else if (prim.IsA<UsdGeomScope>() || xform || prim.GetTypeName().IsEmpty()) {
            // Scope/Xform and untyped grouping prims preserve hierarchy.
        } else {
            return Fail("unsupported prim schema " + prim.GetTypeName().GetString() + ": " +
                        node.name);
        }
        const int id = static_cast<int>(model.nodes.size());
        model.nodes.push_back(std::move(node));
        if (parent >= 0)
            model.nodes[size_t(parent)].children.push_back(id);
        else
            model.scenes.front().nodes.push_back(id);
        return VisitChildren(prim, id);
    }
};

} // namespace

bool LoadUsdFileAsModel(tinygltf::Model& model,
                        const std::filesystem::path& path,
                        std::vector<std::string>& deps) {
    model = tinygltf::Model{};
    deps.clear();
    const UsdStageRefPtr stage = UsdStage::Open(path.string());
    if (!stage)
        return Fail("cannot open composed stage: " + path.string());

    // Relative layer dependencies can be copied together into the
    // project's staging dir and included in its content fingerprint.
    const auto root = std::filesystem::absolute(path).parent_path();
    std::set<std::string> layers;
    for (const auto& layer : stage->GetUsedLayers()) {
        if (!layer || layer == stage->GetRootLayer() || layer == stage->GetSessionLayer())
            continue;
        const std::string real = layer->GetRealPath();
        if (real.empty())
            continue; // anonymous/memory session
        std::error_code ec;
        const auto rel = std::filesystem::relative(real, root, ec);
        if (ec || rel.empty() || rel.is_absolute() || *rel.begin() == "..")
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
        basis.matrix = {scale, 0, 0, 0, 0, 0, -scale, 0, 0, scale, 0, 0, 0, 0, 0, 1};
    } else {
        basis.matrix = {scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, 1};
    }
    model.nodes.push_back(std::move(basis));
    model.scenes.front().nodes.push_back(0);
    if (!cv.VisitChildren(stage->GetPseudoRoot(), 0))
        return false;
    if (model.meshes.empty())
        return Fail("composed stage has no supported meshes");
    cv.blob.Finish();
    return true;
}
