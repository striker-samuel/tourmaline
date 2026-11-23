// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Logical world data channels. Physical streaming stays in WorldPartition.
#include <Engine/WorldPartition/WorldPartition.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine::worldchannels {
using ChannelId = uint64_t;
using WorldId = worldpartition::WorldId;
using WorldCellKey = worldpartition::WorldCellKey;
enum class Domain : uint8_t { Surface2D, Volume3D, SpatialObjects };
enum class ValueKind : uint8_t { Float32, UInt8, Vector3, Opaque };
enum class SampleStatus : uint8_t { Ready, NotResident, Unavailable };

struct ChannelDesc {
    ChannelId id = 0;
    WorldId world = 0;
    std::string name;
    Domain domain = Domain::Surface2D;
    ValueKind values = ValueKind::Opaque;
    worldpartition::StreamLayer streamLayer = worldpartition::StreamLayer::Terrain;
    double cellSizeMeters = worldpartition::kRootCellSize;
    uint32_t samplesPerAxis = 0;
    bool editable = false;
};
inline bool Valid(const ChannelDesc& d) {
    return d.id && d.world && !d.name.empty() &&
           std::isfinite(d.cellSizeMeters) && d.cellSizeMeters > 0 &&
           (d.domain != Domain::Surface2D || d.samplesPerAxis >= 2);
}
class IWorldChannel {
  public:
    virtual ~IWorldChannel() = default;
    virtual const ChannelDesc& Descriptor() const noexcept = 0;
    virtual worldpartition::IWorldStreamProvider* StreamProvider() noexcept { return nullptr; }
};

// WorldPartition borrows registered providers. The host must detach them before
// unregistering channels or destroying the registry.
class WorldChannels {
  public:
    explicit WorldChannels(WorldId world) : world_(world) {}
    bool Register(std::unique_ptr<IWorldChannel> channel) {
        if (!channel || !Valid(channel->Descriptor()) || channel->Descriptor().world != world_)
            return false;
        const auto& d = channel->Descriptor();
        if (channels_.count(d.id) || names_.count(d.name)) return false;
        names_.emplace(d.name, d.id);
        channels_.emplace(d.id, std::move(channel));
        return true;
    }
    IWorldChannel* Find(ChannelId id) const noexcept {
        auto it = channels_.find(id);
        return it == channels_.end() ? nullptr : it->second.get();
    }
    IWorldChannel* Find(std::string_view name) const noexcept {
        auto it = names_.find(std::string(name));
        return it == names_.end() ? nullptr : Find(it->second);
    }
    template<typename T> T* Get(ChannelId id) const noexcept {
        return dynamic_cast<T*>(Find(id));
    }
    bool Unregister(ChannelId id) {
        auto it = channels_.find(id);
        if (it == channels_.end()) return false;
        names_.erase(it->second->Descriptor().name);
        channels_.erase(it);
        return true;
    }
    size_t Count() const noexcept { return channels_.size(); }
    WorldId World() const noexcept { return world_; }
    void AttachProviders(worldpartition::WorldPartition& partition) const {
        for (const auto& [id, channel] : channels_)
            if (auto* p = channel->StreamProvider()) partition.AddProvider(p);
    }
  private:
    WorldId world_;
    std::map<ChannelId, std::unique_ptr<IWorldChannel>> channels_;
    std::map<std::string, ChannelId, std::less<>> names_;
};

// CPU-resident scalar surface data, reusable for height and categorical biomes.
// A host provider owns async IO, persistence, GPU upload and safe retirement.
template<typename T>
class SurfaceChannel final : public IWorldChannel {
    static_assert(std::is_arithmetic_v<T>, "SurfaceChannel requires scalar samples");
  public:
    struct Sample { SampleStatus status = SampleStatus::NotResident; T value{}; };
    explicit SurfaceChannel(ChannelDesc desc) : desc_(std::move(desc)) {}
    const ChannelDesc& Descriptor() const noexcept override { return desc_; }
    bool SetTile(WorldCellKey cell, std::vector<T> data) {
        const size_t n = desc_.samplesPerAxis;
        if (!Valid(desc_) || desc_.domain != Domain::Surface2D ||
            n > std::numeric_limits<size_t>::max() / n || data.size() != n*n) return false;
        tiles_[{cell.x,cell.z}] = std::move(data);
        return true;
    }
    bool EvictTile(WorldCellKey cell) { return tiles_.erase({cell.x,cell.z}) != 0; }
    bool Resident(WorldCellKey cell) const { return tiles_.count({cell.x,cell.z}) != 0; }
    size_t ResidentCount() const noexcept { return tiles_.size(); }
    Sample At(double x, double z) const {
        if (!Valid(desc_) || !std::isfinite(x) || !std::isfinite(z))
            return {SampleStatus::Unavailable, {}};
        const double size = desc_.cellSizeMeters;
        const double cx = std::floor(x/size), cz = std::floor(z/size);
        const double lo = static_cast<double>(INT64_MIN);
        const double hi = std::nextafter(static_cast<double>(INT64_MAX),
                                          -std::numeric_limits<double>::infinity());
        if (cx < lo || cx >= hi || cz < lo || cz >= hi)
            return {SampleStatus::Unavailable, {}};
        auto it = tiles_.find({static_cast<int64_t>(cx),static_cast<int64_t>(cz)});
        if (it == tiles_.end()) return {SampleStatus::NotResident, {}};
        const size_t n = desc_.samplesPerAxis;
        const size_t sx = static_cast<size_t>(
            std::clamp((x/size-cx)*double(n-1), 0.0, double(n-1)));
        const size_t sz = static_cast<size_t>(
            std::clamp((z/size-cz)*double(n-1), 0.0, double(n-1)));
        return {SampleStatus::Ready, it->second[sz*n+sx]};
    }
    bool SetSample(WorldCellKey cell, uint32_t x, uint32_t z, T v) {
        if (!desc_.editable || x >= desc_.samplesPerAxis || z >= desc_.samplesPerAxis)
            return false;
        auto it = tiles_.find({cell.x,cell.z});
        if (it == tiles_.end()) return false;
        it->second[size_t(z)*desc_.samplesPerAxis+x] = v;
        ++revisions_[{cell.x,cell.z}];
        return true;
    }
    uint64_t Revision(WorldCellKey cell) const {
        auto it = revisions_.find({cell.x,cell.z});
        return it == revisions_.end() ? 0 : it->second;
    }
  private:
    using Key = std::pair<int64_t,int64_t>;
    ChannelDesc desc_;
    std::map<Key,std::vector<T>> tiles_;
    std::map<Key,uint64_t> revisions_;
};
using HeightfieldChannel = SurfaceChannel<float>;
using BiomeChannel = SurfaceChannel<uint8_t>;
} // namespace engine::worldchannels
