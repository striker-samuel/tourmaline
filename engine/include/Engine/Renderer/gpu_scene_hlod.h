// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Reference-only GPUScene HLOD traversal. No raster ownership until GPU parity.
#include <array>
#include <vector>
#include <span>
#include <cmath>
#include <cstdint>
#include <limits>
#include <algorithm>
namespace tour::gpu_scene::hlod {
constexpr uint32_t Invalid = UINT32_MAX;
constexpr uint32_t Resident = 1u;
struct alignas(16) Node {
    std::array<float,4> sphere{}; // mesh-local center and radius
    float error = 0;
    uint32_t firstChild = Invalid, childCount = 0;
    uint32_t geometryId = Invalid, flags = 0;
    uint32_t reserved[3]{};
};
static_assert(sizeof(Node) == 48);
enum class Status { Ok, InvalidHierarchy, InvalidView, MissingGeometry, CapacityExceeded };
struct View {
    std::array<double,3> eye{};
    double scale = 1, height = 1080, fov = 1.0471975511965976, pixelBudget = 2;
};
struct Selection { Status status = Status::Ok; std::vector<uint32_t> nodes; };
inline bool Validate(std::span<const Node> nodes) {
    if (nodes.empty()) return false;
    std::vector<uint8_t> parents(nodes.size(),0);
    std::vector<uint32_t> depths(nodes.size(),0); depths[0]=1;
    for (size_t i=0;i<nodes.size();++i) {
        const Node& n=nodes[i];
        if (!depths[i] || depths[i]>64 || !std::isfinite(n.error) || n.error<0 ||
            !std::isfinite(n.sphere[3]) || n.sphere[3]<0) return false;
        for (int c=0;c<3;++c) if (!std::isfinite(n.sphere[c])) return false;
        if ((n.flags & Resident) && n.geometryId==Invalid) return false;
        if (!n.childCount) { if (n.firstChild!=Invalid) return false; continue; }
        if (n.firstChild<=i || n.firstChild>=nodes.size() ||
            n.childCount>nodes.size()-n.firstChild) return false;
        for (uint32_t j=0;j<n.childCount;++j) {
            uint32_t child=n.firstChild+j;
            if (++parents[child]!=1 || nodes[child].error>n.error) return false;
            depths[child]=depths[i]+1;
        }
    }
    if (parents[0]) return false;
    for (size_t i=1;i<nodes.size();++i) if (parents[i]!=1) return false;
    return true;
}
inline Selection Select(std::span<const Node> nodes, const View& view, size_t capacity=1048576) {
    Selection result;
    auto fail=[&](Status s){ result.nodes.clear(); result.status=s; return result; };
    if (!Validate(nodes)) return fail(Status::InvalidHierarchy);
    if (!std::isfinite(view.scale)||view.scale<=0 || !std::isfinite(view.height)||view.height<=0 ||
        !std::isfinite(view.fov)||view.fov<=0 || view.fov>=3.141592653589793 ||
        !std::isfinite(view.pixelBudget)||view.pixelBudget<0) return fail(Status::InvalidView);
    for (double x:view.eye) if (!std::isfinite(x)) return fail(Status::InvalidView);
    double projection=view.height/(2*std::tan(view.fov*0.5));
    const auto visit=[&](auto&& self,uint32_t id)->bool {
        const Node& n=nodes[id]; bool resident=(n.flags&Resident)!=0;
        double dx=double(n.sphere[0])-view.eye[0],dy=double(n.sphere[1])-view.eye[1],
               dz=double(n.sphere[2])-view.eye[2];
        double distance=std::max(0.0,std::hypot(std::hypot(dx,dy),dz)-double(n.sphere[3]));
        double errorPixels=distance>0 ? double(n.error)*view.scale*projection/distance
                                      : std::numeric_limits<double>::infinity();
        if (resident && (n.childCount==0 || (std::isfinite(errorPixels) &&
            errorPixels<=view.pixelBudget))) { result.nodes.push_back(id); return true; }
        if (n.childCount) {
            size_t start=result.nodes.size(); bool complete=true;
            for (uint32_t i=0;i<n.childCount;++i)
                if (!self(self,n.firstChild+i)) { complete=false; break; }
            if (complete) return true;
            result.nodes.resize(start); // atomic parent fallback: no half-refined subtree
        }
        if (!resident) return false;
        result.nodes.push_back(id); return true;
    };
    if (!visit(visit,0)) return fail(Status::MissingGeometry);
    if (result.nodes.size()>capacity) return fail(Status::CapacityExceeded);
    return result;
}
} // namespace tour::gpu_scene::hlod
