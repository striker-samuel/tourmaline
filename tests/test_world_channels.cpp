// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/WorldPartition/WorldChannels.h>
#include <iostream>
#include <memory>
#include <vector>

namespace wc = engine::worldchannels;
namespace wp = engine::worldpartition;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        if (!ok) ++failures;
    };
    wc::WorldChannels channels(11);
    wc::ChannelDesc height{100, 11, "terrain.height", wc::Domain::Surface2D,
        wc::ValueKind::Float32, wp::StreamLayer::Terrain, 256.0, 3, true};
    wc::ChannelDesc biome{200, 11, "ecology.biome", wc::Domain::Surface2D,
        wc::ValueKind::UInt8, wp::StreamLayer::Terrain, 64.0, 2, true};
    check(!channels.Register(nullptr), "null channel rejected");
    auto wrong = height; wrong.world = 12;
    check(!channels.Register(std::make_unique<wc::HeightfieldChannel>(wrong)), "foreign world rejected");
    check(channels.Register(std::make_unique<wc::HeightfieldChannel>(height)), "height registered");
    check(!channels.Register(std::make_unique<wc::HeightfieldChannel>(height)), "duplicate ID rejected");
    auto dup = biome; dup.name = height.name;
    check(!channels.Register(std::make_unique<wc::BiomeChannel>(dup)), "duplicate name rejected");
    check(channels.Register(std::make_unique<wc::BiomeChannel>(biome)), "biome registered");
    check(channels.Count() == 2 && channels.Find("terrain.height") == channels.Find(100),
          "lookup by stable ID and name");
    check(channels.Get<wc::BiomeChannel>(100) == nullptr, "typed mismatch rejected");
    auto* h = channels.Get<wc::HeightfieldChannel>(100);
    auto* b = channels.Get<wc::BiomeChannel>(200);
    check(h && b, "typed lookup");
    if (h && b) {
        check(h->SetTile({-1,0}, {1,2,3,4,5,6,7,8,9}), "height tile resident");
        check(!h->SetTile({0,0}, {1}), "invalid tile sample count rejected");
        check(b->SetTile({-4,0}, {4,5,6,7}), "independent biome tile resident");
        auto lhs = h->At(-256.0,0.0);
        auto rhs = h->At(-1.0,0.0);
        check(lhs.status == wc::SampleStatus::Ready && lhs.value == 1,
              "negative cell floor at boundary");
        check(rhs.status == wc::SampleStatus::Ready && rhs.value == 2,
              "height sample at right edge");
        check(b->At(-256.0,0.0).value == 4, "biome independent cell dimensions");
        check(h->At(10,10).status == wc::SampleStatus::NotResident, "missing tile not height zero");
        check(h->SetSample({-1,0},0,0,0.0f), "edit sample");
        check(h->At(-256.0,0.0).status == wc::SampleStatus::Ready &&
              h->At(-256.0,0.0).value == 0.0f, "zero height is valid");
        check(h->Revision({-1,0}) == 1, "dirty revision tracked");
        check(!h->SetSample({-1,0},3,0,1.0f), "out-of-range edit rejected");
        check(h->EvictTile({-1,0}) && h->Revision({-1,0}) == 1,
              "eviction preserves revision");
        check(h->At(-256.0,0.0).status == wc::SampleStatus::NotResident,
              "eviction removes resident sample");
    }
    check(channels.Unregister(100) && channels.Find(100) == nullptr &&
          channels.Find("terrain.height") == nullptr, "unregister clears both indexes");
    check(!channels.Unregister(100) && channels.Count() == 1, "unknown unregister no-op");
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
