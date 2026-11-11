// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/gpu_scene_hlod.h>
#include <cstdio>
using namespace tour::gpu_scene::hlod;
int checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"HLOD fail line %d: %s\n",__LINE__,#x); } } while(0)
Node Make(float e,uint32_t f,uint32_t c,bool r,uint32_t g) {
    Node n{}; n.sphere={0,0,0,1}; n.error=e; n.firstChild=f; n.childCount=c;
    n.flags=r?Resident:0; n.geometryId=g; return n;
}
int main() {
    std::vector<Node> tree{Make(8,1,2,true,7),Make(0,Invalid,0,true,8),
                           Make(0,Invalid,0,true,9)};
    View near{}; near.eye={0,0,5}; near.pixelBudget=1;
    CHECK(Validate(tree));
    CHECK(Select(tree,near).nodes==std::vector<uint32_t>({1,2}));
    View far=near; far.eye={0,0,100000};
    CHECK(Select(tree,far).nodes==std::vector<uint32_t>({0}));
    tree[2].flags=0;
    CHECK(Select(tree,near).nodes==std::vector<uint32_t>({0}));
    tree[0].flags=0;
    CHECK(Select(tree,near).status==Status::MissingGeometry);
    tree[2].flags=Resident;
    CHECK(Select(tree,near).nodes==std::vector<uint32_t>({1,2}));
    CHECK(Select(tree,near,1).status==Status::CapacityExceeded);
    tree[0].firstChild=0; CHECK(!Validate(tree)); tree[0].firstChild=1;
    tree[2].error=9; CHECK(!Validate(tree)); tree[2].error=0;
    tree[2].sphere[3]=-1; CHECK(!Validate(tree)); tree[2].sphere[3]=1;
    tree[0].geometryId=Invalid; tree[0].flags=Resident; CHECK(!Validate(tree));
    tree[0].geometryId=7;
    near.fov=0; CHECK(Select(tree,near).status==Status::InvalidView);
    std::printf("HLOD checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
