// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/gpu_scene_pipeline_registry.h>
#include <array>
#include <cstdio>
#include <vector>
using namespace tour::gpu_scene;
static int checks = 0, failures = 0;
#define CHECK(X) do { ++checks; if (!(X)) { ++failures; std::fprintf(stderr,"FAIL:%d %s\n",__LINE__,#X); } } while(false)
int main() {
    PipelineRegistry registry;
    const PipelineKey stock{1,10,100,PipelineCoverage::Opaque,PipelineCull::Back};
    const PipelineKey hologram{2,20,100,PipelineCoverage::Opaque,PipelineCull::Back};
    const PipelineKey masked{1,10,100,PipelineCoverage::Masked,PipelineCull::None};
    const auto a = registry.Intern(stock), b = registry.Intern(hologram),
               c = registry.Intern(masked);
    CHECK(a && b && c && *a != *b && *b != *c);
    CHECK(registry.Intern(stock) == a);
    CHECK(!registry.Intern({0,20,100,PipelineCoverage::Opaque,PipelineCull::Back}));
    CHECK(!registry.Lookup(3));
    const std::array<RegisteredPipelineDraw,6> registered{{
        {*a,11},{*b,12},{*a,13},{*c,14},{*a,15},{*b,16}}};
    PipelineCapacityPlan capacity;
    CHECK(PrecountPipelineBins(registered,registry,capacity));
    CHECK(capacity.capacity == 6);
    CHECK(capacity.bins[*a].capacity == 3);
    CHECK(capacity.bins[*b].offset == 3 && capacity.bins[*b].capacity == 2);
    CHECK(capacity.bins[*c].offset == 5 && capacity.bins[*c].capacity == 1);
    std::array<uint32_t,6> output{};
    PipelineFlightLayout frame;
    CHECK(PackPipelineFlight(registered,registry,capacity,output,frame,5));
    CHECK(ValidPipelineFlight(frame));
    CHECK((output == std::array<uint32_t,6>{11,13,15,12,16,14}));
    const std::array<RegisteredPipelineDraw,3> visible{{{*b,12},{*a,13},{*c,14}}};
    CHECK(PackPipelineFlight(visible,registry,capacity,output,frame,6));
    CHECK(frame.drawCount == 3 && frame.drawCapacity == 6);
    CHECK(frame.bins[*b].count == 1 && output[3] == 12);
    CHECK(frame.bins[*a].count == 1 && output[0] == 13);
    CHECK(frame.bins[*c].count == 1 && output[5] == 14);
    const auto oldFrame = frame;
    const auto oldOutput = output;
    const std::array<RegisteredPipelineDraw,1> invalid{{{999,90}}};
    CHECK(!PackPipelineFlight(invalid,registry,capacity,output,frame,7));
    CHECK(frame.generation == oldFrame.generation && output == oldOutput);
    PipelineCapacityPlan oldCapacity = capacity;
    CHECK(!PrecountPipelineBins(invalid,registry,capacity));
    CHECK(capacity.capacity == oldCapacity.capacity);
    auto broken = capacity;
    broken.bins[1].offset = 9;
    CHECK(!PackPipelineFlight(visible,registry,broken,output,frame,7));
    CHECK(frame.generation == 6 && output == oldOutput);
    const std::array<RegisteredPipelineDraw,4> overflow{{
        {*b,1},{*b,2},{*b,3},{*a,4}}};
    CHECK(!PackPipelineFlight(overflow,registry,capacity,output,frame,7));
    CHECK(output == oldOutput);
    PipelineRegistry emptyRegistry;
    PipelineCapacityPlan emptyPlan;
    CHECK(PrecountPipelineBins({},emptyRegistry,emptyPlan));
    PipelineFlightLayout emptyFrame;
    CHECK(PackPipelineFlight({},emptyRegistry,emptyPlan,{},emptyFrame,1));
    CHECK(ValidPipelineFlight(emptyFrame));
    std::fprintf(stdout,"GpuScenePipelineRegistryTests: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
