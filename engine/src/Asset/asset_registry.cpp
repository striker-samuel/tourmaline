// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/asset_registry.h>
// GetMeshHeapVertexBase reads StaticMeshAllocation::vertexBase through the
// forward-declared pointer — the heap header carries the full definition.
#include <Engine/Renderer/static_vertex_heap.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace tour {
using namespace ::smvk;

// ---------------------------------------------------------------------------
// Asset URI normalization (view-based; see asset_registry.h for semantics).
// Fast path: already-normalized input returns the caller's view unchanged.
// Slow path: mirrors vfs::Normalize (separator collapsing, '.' skipping,
// '..' root-escape rejection, drive-letter/leading-slash rejection) but
// builds the result directly into the caller's scratch arena instead of
// round-tripping std::string.
// ---------------------------------------------------------------------------

} // namespace tour
namespace smvk {
// (smvk-scope: the asset-registry free functions stay in smvk - the
// header declares them there; only the WorldPage members are tour::.)
// NOTE: the UNQUALIFIED function name - GCC 14 rejects a qualified-id
// in a declaration here ("explicit qualification in declaration")
// while MSVC accepts it; the unqualified form defines the same
// smvk::NormalizeAssetUri (the enclosing namespace is the qualifier).
smvk::StringViewT NormalizeAssetUri(StringViewT uri, Arena& scratch) {
    if (uri.empty())
        return {};
    if (UriAlreadyNormalized(uri))
        return uri; // includes the '#' fragment verbatim (contract)

    const auto [file, frag] = SplitAssetUri(uri);
    if (file.empty())
        return {}; // "#frag" has no file part

    // Regular paths reject drive-letter/leading-slash inputs.
    if (file.size() >= 2 && file[1] == ':')
        return {}; // drive-letter absolute
    if (file[0] == '/' || file[0] == '\\')
        return {}; // posix absolute
    StringViewT body = file;

    // Segment pass: collapse separators, skip '.', reject "..".
    // At most (n + 1) / 2 segments in a path of length n.
    // Borrow the caller's arena instead of allocating a heap vector.
    const size_t maxParts = (body.size() + 1) / 2;
    auto* parts = static_cast<StringViewT*>(
        scratch.Allocate(maxParts * sizeof(StringViewT), alignof(StringViewT)));
    size_t partCount = 0;
    size_t start = 0;
    for (size_t i = 0; i <= body.size(); ++i) {
        if (i == body.size() || body[i] == '/' || body[i] == '\\') {
            const StringViewT seg = body.substr(start, i - start);
            start = i + 1;
            if (seg.empty() || (seg.size() == 1 && seg[0] == '.'))
                continue;
            if (seg.size() == 2 && seg[0] == '.' && seg[1] == '.')
                return {}; // escaped the virtual root
            std::construct_at(parts + partCount++, seg);
        }
    }
    if (partCount == 0)
        return {};

    size_t total = 1; // NUL
    for (size_t i = 0; i < partCount; ++i) {
        if (i)
            total += 1;
        total += parts[i].size();
    }
    if (!frag.empty())
        total += 1 + frag.size();

    char* buf = static_cast<char*>(scratch.Allocate(total));
    size_t off = 0;
    for (size_t i = 0; i < partCount; ++i) {
        if (i)
            buf[off++] = '/';
        std::memcpy(buf + off, parts[i].data(), parts[i].size());
        off += parts[i].size();
    }
    if (!frag.empty()) {
        buf[off++] = '#';
        std::memcpy(buf + off, frag.data(), frag.size());
        off += frag.size();
    }
    buf[off] = '\0';
    return StringViewT(buf, off);
}
} // namespace smvk

namespace tour {
using namespace ::smvk;

// ---------------------------------------------------------------------------
// WorldPage URI registry: registration normalizes into the cluster-owned
// arena (keys stay valid until the cluster dies); lookups normalize into
// the frame arena (fast path: zero allocation) and probe the UriMap.
// ---------------------------------------------------------------------------

bool WorldPage::RegisterMeshUri(StringViewT uri, WorldPageStaticMeshHandle handle) {
    const StringViewT n = NormalizeAssetUri(uri, uriArena_);
    if (n.empty() || !handle.valid())
        return false;
    const StringViewT key = uriArena_.AllocateString(n);
    meshByUri_.Insert(key, handle);
    meshUriLedger_.emplace_back(handle, key);
    return true;
}

bool WorldPage::RegisterMaterialUri(StringViewT uri, WorldPageMaterialHandle handle) {
    const StringViewT n = NormalizeAssetUri(uri, uriArena_);
    if (n.empty() || !handle.valid())
        return false;
    const StringViewT key = uriArena_.AllocateString(n);
    materialByUri_.Insert(key, handle);
    materialUriLedger_.emplace_back(handle, key);
    return true;
}

bool WorldPage::RegisterLightUri(StringViewT uri, WorldPageLightHandle handle) {
    const StringViewT n = NormalizeAssetUri(uri, uriArena_);
    if (n.empty() || !handle.valid())
        return false;
    const StringViewT key = uriArena_.AllocateString(n);
    lightByUri_.Insert(key, handle);
    lightUriLedger_.emplace_back(handle, key);
    return true;
}

bool WorldPage::RegisterCameraUri(StringViewT uri, WorldPageCameraHandle handle) {
    const StringViewT n = NormalizeAssetUri(uri, uriArena_);
    if (n.empty() || !handle.valid())
        return false;
    const StringViewT key = uriArena_.AllocateString(n);
    cameraByUri_.Insert(key, handle);
    cameraUriLedger_.emplace_back(handle, key);
    return true;
}

bool WorldPage::RegisterDecalUri(StringViewT uri, WorldPageDecalHandle handle) {
    const StringViewT n = NormalizeAssetUri(uri, uriArena_);
    if (n.empty() || !handle.valid())
        return false;
    const StringViewT key = uriArena_.AllocateString(n);
    decalByUri_.Insert(key, handle);
    decalUriLedger_.emplace_back(handle, key);
    return true;
}

void WorldPage::RegisterGiBinding(GiBinding::Kind kind, const std::string& name) {
    if (!name.empty())
        giBindings.push_back(GiBinding{kind, name});
}

void WorldPage::AddBakePayload(const std::string& vfsPath, std::vector<uint8_t> bytes) {
    if (vfsPath.empty() || bytes.empty())
        return;
    BakePayload p;
    p.owned = std::move(bytes);
    bakePayloads_[vfsPath] = std::move(p);
}

void WorldPage::AddBakePayloadBorrowed(const std::string& vfsPath, std::span<const uint8_t> bytes) {
    if (vfsPath.empty() || bytes.empty())
        return;
    BakePayload p;
    p.borrowed = bytes;
    bakePayloads_[vfsPath] = std::move(p);
}

const WorldPage::BakePayload* WorldPage::FindBakePayload(const std::string& vfsPath) const {
    const auto it = bakePayloads_.find(vfsPath);
    if (it == bakePayloads_.end())
        return nullptr;
    return &it->second;
}

bool WorldPage::RegisterTextureUri(StringViewT uri, TextureHandle handle) {
    const StringViewT n = NormalizeAssetUri(uri, uriArena_);
    if (n.empty() || !handle.valid())
        return false;
    textureByUri_.Insert(uriArena_.AllocateString(n), handle);
    if (world) ++world->materialTextureRegistryRevision;
    return true;
}

void WorldPage::UnregisterUri(StringViewT uri) {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return;
    meshByUri_.Erase(n);
    materialByUri_.Erase(n);
    lightByUri_.Erase(n);
    cameraByUri_.Erase(n);
    decalByUri_.Erase(n);
    textureByUri_.Erase(n);
    if (world) ++world->materialTextureRegistryRevision;
    textureSamplers_.Erase(n);
    textureSources_.Erase(n);
    meshLods_.Erase(n);
    meshMeshlets_.Erase(n);
}

std::optional<WorldPageStaticMeshHandle> WorldPage::FindMeshByUri(StringViewT uri) const {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return std::nullopt;
    const WorldPageStaticMeshHandle* hit = meshByUri_.Find(n);
    if (!hit)
        return std::nullopt;
    return *hit;
}

std::optional<WorldPageMaterialHandle> WorldPage::FindMaterialByUri(StringViewT uri) const {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return std::nullopt;
    const WorldPageMaterialHandle* hit = materialByUri_.Find(n);
    if (!hit)
        return std::nullopt;
    return *hit;
}

std::optional<WorldPageLightHandle> WorldPage::FindLightByUri(StringViewT uri) const {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return std::nullopt;
    const WorldPageLightHandle* hit = lightByUri_.Find(n);
    if (!hit)
        return std::nullopt;
    return *hit;
}

std::optional<WorldPageCameraHandle> WorldPage::FindCameraByUri(StringViewT uri) const {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return std::nullopt;
    const WorldPageCameraHandle* hit = cameraByUri_.Find(n);
    if (!hit)
        return std::nullopt;
    return *hit;
}

std::optional<WorldPageDecalHandle> WorldPage::FindDecalByUri(StringViewT uri) const {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return std::nullopt;
    const WorldPageDecalHandle* hit = decalByUri_.Find(n);
    if (!hit)
        return std::nullopt;
    return *hit;
}

std::optional<TextureHandle> WorldPage::FindTextureByUri(StringViewT uri) const {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return std::nullopt;
    const TextureHandle* hit = textureByUri_.Find(n);
    if (!hit)
        return std::nullopt;
    return *hit;
}

StringViewT WorldPage::GetMeshUri(WorldPageStaticMeshHandle handle) const {
    if (!handle.valid())
        return {};
    // Ledger newest-first (see meshUriLedger_): the forward map keeps
    // only the newest handle per uri, so a shared-asset scene's earlier
    // occurrences resolve here — through their own registration rows.
    for (auto it = meshUriLedger_.rbegin(); it != meshUriLedger_.rend(); ++it)
        if (it->first == handle)
            return it->second;
    return {};
}

StringViewT WorldPage::GetMaterialUri(WorldPageMaterialHandle handle) const {
    if (!handle.valid())
        return {};
    for (auto it = materialUriLedger_.rbegin(); it != materialUriLedger_.rend(); ++it)
        if (it->first == handle)
            return it->second;
    return {};
}

StringViewT WorldPage::GetLightUri(WorldPageLightHandle handle) const {
    if (!handle.valid())
        return {};
    for (auto it = lightUriLedger_.rbegin(); it != lightUriLedger_.rend(); ++it)
        if (it->first == handle)
            return it->second;
    return {};
}

StringViewT WorldPage::GetCameraUri(WorldPageCameraHandle handle) const {
    if (!handle.valid())
        return {};
    for (auto it = cameraUriLedger_.rbegin(); it != cameraUriLedger_.rend(); ++it)
        if (it->first == handle)
            return it->second;
    return {};
}

StringViewT WorldPage::GetDecalUri(WorldPageDecalHandle handle) const {
    if (!handle.valid())
        return {};
    for (auto it = decalUriLedger_.rbegin(); it != decalUriLedger_.rend(); ++it)
        if (it->first == handle)
            return it->second;
    return {};
}

WorldPageLightHandle WorldPage::CreateLight(SceneLight def) {
    return lights.create(std::move(def));
}

WorldPageCameraHandle WorldPage::CreateCamera(SceneCamera def) {
    return cameras.create(std::move(def));
}

WorldPageDecalHandle WorldPage::CreateDecal(SceneDecal def) {
    return decals.create(std::move(def));
}

void WorldPage::RegisterTextureSampler(StringViewT uri, const TextureSamplerDesc& desc) {
    const StringViewT n = NormalizeAssetUri(uri, uriArena_);
    if (n.empty())
        return;
    textureSamplers_.Insert(uriArena_.AllocateString(n), desc);
}

std::optional<TextureSamplerDesc> WorldPage::GetTextureSampler(StringViewT uri) const {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return std::nullopt;
    const TextureSamplerDesc* hit = textureSamplers_.Find(n);
    if (!hit)
        return std::nullopt;
    return *hit;
}

void WorldPage::SetTextureSource(StringViewT uri, std::vector<uint8_t> bytes) {
    const StringViewT n = NormalizeAssetUri(uri, uriArena_);
    if (n.empty())
        return;
    textureSources_.Insert(uriArena_.AllocateString(n), std::move(bytes));
}

const std::vector<uint8_t>* WorldPage::GetTextureSource(StringViewT uri) const {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return nullptr;
    return textureSources_.Find(n);
}

void WorldPage::ReleaseTextureSources() {
    textureSources_.Clear();
}

void WorldPage::PinPakImage(std::shared_ptr<const tour::asset::PakImage> image) {
    pakImagePin_ = std::move(image);
}

void WorldPage::SetRetainUploads(bool retain) {
    retainUploads_ = retain;
    if (!retain)
        retainedUploads_.clear();
}

void WorldPage::SetOfflineMode(bool offline) {
    offlineMode_ = offline;
    if (offline)
        SetRetainUploads(true);
}

uint32_t WorldPage::AllocOfflineTextureId() {
    return offlineNextTextureId_++;
}

const MeshData* WorldPage::GetRetainedUpload(WorldPageStaticMeshHandle handle) const {
    if (!handle.valid())
        return nullptr;
    const auto it = retainedUploads_.find(handle.index());
    if (it == retainedUploads_.end())
        return nullptr;
    return &it->second;
}

uint32_t WorldPage::GetMeshHeapVertexBase(WorldPageStaticMeshHandle handle) const {
    if (!handle.valid())
        return 0xFFFFFFFFu;
    const auto it = meshAllocations_.find(handle.index());
    if (it == meshAllocations_.end())
        return 0xFFFFFFFFu;
    return static_cast<uint32_t>(it->second->vertexBase);
}

void WorldPage::RegisterMeshLod(StringViewT uri, const LodInfo& lod) {
    const StringViewT n = NormalizeAssetUri(uri, uriArena_);
    if (n.empty())
        return;
    meshLods_.Insert(uriArena_.AllocateString(n), lod);
}

std::optional<LodInfo> WorldPage::GetMeshLod(StringViewT uri) const {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return std::nullopt;
    const LodInfo* hit = meshLods_.Find(n);
    if (!hit)
        return std::nullopt;
    return *hit;
}

std::vector<std::pair<StringViewT, LodInfo>> WorldPage::ListMeshLods() const {
    std::vector<std::pair<StringViewT, LodInfo>> out;
    out.reserve(meshLods_.Size());
    meshLods_.ForEach([&](StringViewT uri, const LodInfo& lod) { out.emplace_back(uri, lod); });
    std::sort(
        out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

void WorldPage::RegisterMeshMeshlets(StringViewT uri, const CookedMeshlets& meshlets) {
    const StringViewT n = NormalizeAssetUri(uri, uriArena_);
    if (n.empty() || meshlets.empty())
        return;
    meshMeshlets_.Insert(uriArena_.AllocateString(n), meshlets);
}

std::optional<CookedMeshlets> WorldPage::GetMeshMeshlets(StringViewT uri) const {
    const StringViewT n = NormalizeAssetUri(uri);
    if (n.empty())
        return std::nullopt;
    const CookedMeshlets* hit = meshMeshlets_.Find(n);
    if (!hit)
        return std::nullopt;
    return *hit;
}

std::vector<std::pair<StringViewT, TextureHandle>> WorldPage::ListTextureUris() const {
    std::vector<std::pair<StringViewT, TextureHandle>> out;
    out.reserve(textureByUri_.Size());
    textureByUri_.ForEach(
        [&](StringViewT uri, const TextureHandle& h) { out.emplace_back(uri, h); });
    std::sort(
        out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

} // namespace tour
