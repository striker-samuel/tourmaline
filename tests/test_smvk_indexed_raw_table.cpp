// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Fixed-world-slot material publication, no GPU required.
#include <Engine/RHI/indexed_raw_table.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
static int failures=0;
#define CHECK(x) do { if(!(x)){ ++failures; std::fprintf(stderr,"FAIL %s:%d %s\\n",__FILE__,__LINE__,#x); } } while(0)
struct alignas(16) Material {
    uint32_t id=0;
    uint32_t slot=0;
    uint32_t bindless=0;
    uint32_t flags=0;
};
int main() {
    smvk::table::IndexedRawTable<Material,3> table;
    std::array<std::vector<Material>,3> mirrors{};
    std::array<uint32_t,3> writes{};
    auto publish=[&](uint32_t f,bool full=false,bool fail=false){
        return table.Publish(f,full,[&](size_t off,const void* bytes,size_t count){
            if(fail)return false;
            CHECK(off % sizeof(Material)==0 && count % sizeof(Material)==0);
            auto& dst=mirrors[f];
            if(dst.size()<table.Size())dst.resize(table.Size());
            std::memcpy(reinterpret_cast<char*>(dst.data())+off,bytes,count);
            ++writes[f];
            return true;
        });
    };
    const std::array<Material,3> initial={{{10,0,21,0},{},{30,2,48,0}}};
    CHECK(table.Sync(initial));
    CHECK(table.Size()==3);
    CHECK(!publish(0,true,true));
    CHECK(table.PendingRows(0)==3);
    for(uint32_t f=0;f<3;++f)CHECK(publish(f,true));
    for(auto& v:mirrors) CHECK(v.size()==3 && v[0].id==10 && v[1].id==0 && v[2].id==30);
    writes.fill(0);
    for(uint32_t f=0;f<3;++f) CHECK(publish(f));
    CHECK(writes[0]==0 && writes[1]==0 && writes[2]==0);
    // One changed material or bindless mapping affects one row per flight.
    auto changed=initial;
    changed[2].bindless=99;
    CHECK(table.Sync(changed));
    CHECK(table.PendingRows(0)==1 && table.PendingRows(1)==1 && table.PendingRows(2)==1);
    CHECK(publish(0));
    CHECK(mirrors[0][2].bindless==99 && mirrors[1][2].bindless==48);
    // Scene unload must clear old tail slots on ALL flights.
    CHECK(table.Sync(std::span<const Material>(changed.data(),1)));
    for(uint32_t f=0;f<3;++f)CHECK(publish(f));
    for(auto& v:mirrors)CHECK(v[2].id==0 && v[1].id==0 && v[0].id==10);
    // Reuse a hole without shifting the identity of any later slot.
    changed[2]={75,2,37,0};
    CHECK(table.Sync(changed));
    for(uint32_t f=0;f<3;++f)CHECK(publish(f));
    for(auto& v:mirrors)CHECK(v[2].id==75 && v[2].bindless==37);
    writes.fill(0);
    CHECK(table.Sync(changed));
    for(uint32_t f=0;f<3;++f)CHECK(publish(f));
    CHECK(writes[0]==0 && writes[1]==0 && writes[2]==0);
    table.Reset();
    CHECK(table.Size()==0);
    CHECK(table.Sync(initial));
    for(uint32_t f=0;f<3;++f)CHECK(publish(f,true));
    for(auto& v:mirrors)CHECK(v[2].id==30);
    std::printf("indexed-raw-table failures=%d\\n",failures);
    return failures?1:0;
}
