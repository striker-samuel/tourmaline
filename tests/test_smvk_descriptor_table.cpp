// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Experimental no-BDA publication model. No Vulkan device required.
#include <Engine/RHI/descriptor_table_mirror.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

struct Payload { uint32_t x = 0, y = 0; };
struct FakeBuffer {
    std::vector<uint8_t> bytes;
    uint32_t id = 0;
};

int main() {
    smvk::table::RawTable<Payload,3> table;
    smvk::table::DescriptorTableMirror<Payload,3,FakeBuffer> mirror;
    uint32_t created = 0;
    std::array<uint32_t,3> copies{};
    auto create = [&](size_t size) -> std::optional<FakeBuffer> {
        return FakeBuffer{std::vector<uint8_t>(size, 0xff), ++created};
    };
    auto upload = [&](uint32_t flight, bool fail) {
        return mirror.Publish(table,flight,create,
            [&](const FakeBuffer& dst, size_t offset,
                const void* src, size_t bytes) -> bool {
                ++copies[flight];
                if (fail) return false;
                auto& writable = const_cast<FakeBuffer&>(dst);
                if (offset + bytes > writable.bytes.size()) return false;
                std::memcpy(writable.bytes.data() + offset, src, bytes);
                return true;
            });
    };
    CHECK(!mirror.GetBinding(table,0));
    auto a = table.Insert(Payload{11,22});
    auto b = table.Insert(Payload{33,44});
    CHECK(!upload(0,true));
    CHECK(!mirror.GetBinding(table,0));
    CHECK(upload(0,false));
    CHECK(upload(1,false));
    CHECK(upload(2,false));
    const auto first = mirror.GetBinding(table,0);
    CHECK(first && first.rowCount==2 && first.capacityBytes>=64*1024);
    CHECK(first.allocationRevision==1);
    CHECK(created==3);
    CHECK(!mirror.GetBinding(table,3));
    auto read = [&](uint32_t flight,uint32_t idx) {
        Payload out{};
        auto binding=mirror.GetBinding(table,flight);
        if (binding) std::memcpy(&out,binding.buffer->bytes.data()+idx*sizeof(Payload),sizeof(out));
        return out;
    };
    for (uint32_t f=0;f<3;++f) CHECK(read(f,a.slot).x==11 && read(f,b.slot).x==33);
    CHECK(table.Update(a,Payload{99,100}));
    copies.fill(0);
    CHECK(upload(0,false));
    CHECK(copies[0]==1);
    CHECK(table.Update(b,Payload{55,66})); // live B updates normally
    CHECK(upload(1,false));
    CHECK(upload(2,false));
    for (uint32_t f=0;f<3;++f) CHECK(read(f,a.slot).x==99);
    CHECK(table.Erase(a));
    CHECK(upload(2,false));
    CHECK(read(2,a.slot).x==0);
    auto c=table.Insert(Payload{77,88});
    CHECK(c.slot==a.slot && c.generation!=a.generation);
    CHECK(!table.Update(a,Payload{1,2}));
    CHECK(upload(0,false)); CHECK(upload(1,false)); CHECK(upload(2,false));
    for(uint32_t f=0;f<3;++f) CHECK(read(f,c.slot).x==77);
    copies.fill(0);
    CHECK(upload(0,false) && copies[0]==0);
    CHECK(mirror.GetBinding(table,0).allocationRevision==1);
    // Cross the first 64-KiB boundary. A failed growth allocation must
    // withhold the undersized old descriptor instead of exposing an OOB view.
    while (table.Size() <= 8192)
        CHECK(bool(table.Insert(Payload{uint32_t(table.Size()),1})));
    auto failCreate = [&](size_t) -> std::optional<FakeBuffer> {
        return std::nullopt;
    };
    CHECK(!mirror.Publish(table,0,failCreate,
        [&](const FakeBuffer&,size_t,const void*,size_t){ return true; }));
    CHECK(!mirror.GetBinding(table,0));
    CHECK(upload(0,false));
    CHECK(mirror.GetBinding(table,0).capacityBytes>=128*1024);
    CHECK(mirror.GetBinding(table,0).allocationRevision==2);
    CHECK(read(0,c.slot).x==77);
    // The adapter is descriptor-backed only: no device addresses, no textures.
    std::printf("descriptor-table checks complete failures=%d\n",failures);
    return failures ? 1 : 0;
}
