// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/RHI/raw_table.h>
#include <Engine/Renderer/texture_gpu_records.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

struct Tiny { uint32_t a=0, b=0; };
struct alignas(16) Big { uint32_t a=0, b=0, c=0, d=0; };
int failures=0;
#define CHECK(x) do { if (!(x)) { ++failures; std::fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

template <typename T> void TestRaw() {
    smvk::table::RawTable<T,3> table;
    std::array<std::vector<T>,3> mirror{};
    auto publish=[&](int flight, bool full=false, int failAt=-1) {
        int writes=0;
        return table.Publish(flight,full,[&](size_t off,const void* src,size_t bytes) {
            if (writes++ == failAt) return false;
            CHECK(off % sizeof(T)==0 && bytes % sizeof(T)==0);
            CHECK(off+bytes <= size_t(table.Size())*sizeof(T));
            if (mirror[flight].size()<table.Size()) mirror[flight].resize(table.Size());
            if (bytes) std::memcpy(reinterpret_cast<unsigned char*>(mirror[flight].data())+off,src,bytes);
            return true;
        });
    };
    T a{}; a.a=11;
    T b{}; b.a=22;
    auto x=table.Insert(a);
    auto y=table.Insert(b);
    CHECK(table.Contains(x) && table.Contains(y));
    CHECK(!publish(0,true,0)); // failed full snapshot must remain pending
    CHECK(publish(0,true));
    CHECK(mirror[0][x.slot].a==11 && mirror[0][y.slot].a==22);
    a.a=33;
    CHECK(table.Update(x,a));
    CHECK(publish(1,true));
    CHECK(table.Erase(y));
    CHECK(!table.Contains(y));
    auto z=table.Insert(b);
    CHECK(z.slot==y.slot && z.generation!=y.generation);
    CHECK(!table.Update(y,a));
    CHECK(publish(2,true));
    CHECK(publish(0));
    CHECK(publish(1));
    for (auto& rows:mirror) {
        CHECK(rows.size()==2);
        CHECK(rows[x.slot].a==33 && rows[z.slot].a==22);
    }
    for (int flight=0;flight<3;++flight) {
        CHECK(table.PendingRows(flight)==0);
        int count=0;
        CHECK(table.Publish(flight,false,[&](size_t,const void*,size_t){++count; return true;}));
        CHECK(count==0);
    }
    CHECK(table.Erase(x));
    CHECK(table.Erase(z));
    for(int f=0;f<3;++f) {
        CHECK(publish(f));
        CHECK(mirror[f][x.slot].a==0 && mirror[f][z.slot].a==0);
    }
}
void CheckTextureRecords() {
    tour::gpu_records::TextureMetadata rows;
    tour::gfx::TextureMeta m{}; m.longSide=1024; m.fullMips=11;
    const auto id=rows.Insert(m);
    CHECK(rows.Contains(id));
    CHECK(rows.Get(id)->longSide==1024);
    m.fullMips=7;
    CHECK(rows.Update(id,m));
    CHECK(rows.Get(id)->fullMips==7);
    CHECK(rows.Erase(id));
    CHECK(!rows.Contains(id));
}

int main() {
    CheckTextureRecords();
    TestRaw<Tiny>(); // 8-byte texture-like ABI
    TestRaw<Big>();  // 16-byte and wider ABI
    std::printf("raw_table failures=%d\n",failures);
    return failures?1:0;
}
