// placement_mesh_tile.cs.hlsl — GPU tile evaluation, mesh-surface domain.
// Mirrors CPU EvaluateTile() for mesh layers: alias-table triangle pick +
// barycentric sample, density field, quantized accept, weighted prototype,
// yaw + uniform scale, atomic append. customB carries the candidate index
// for the downstream constraint passes.
//
// Slope-to-rock and CPU min-distance have no GPU form yet: run them as a
// post readback step, or wait for the constraint kernels.

#include "placement_common.hlsli"
#include "placement_field.hlsli"
#include "placement_domains.hlsli"

// Must match C++ PlacementScatterPush (64 bytes).
struct PlacementScatterPush
{
    vk::BufferPointer<PlacementInstanceBuffer> outInstances; // 0
    vk::BufferPointer<PlacementCountBuffer> outCount;        // 8
    vk::BufferPointer<PlacementDomainParams> prm;            // 16
    uint64_t pad0;                                           // 24
    uint seedLo;      // 32
    uint seedHi;      // 36
    uint nodeId;      // 40
    uint candidateCount; // 44
    int3 tile;        // 48
    uint flags;       // 60
};

[[vk::push_constant]]
ConstantBuffer<PlacementScatterPush> pc;

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    uint i = groupId.x * 64 + threadId.x;
    if (i >= pc.candidateCount)
        return;

    uint4 id = PlacementMakeStableID(pc.seedLo, pc.seedHi, pc.nodeId,
        pc.tile, 0, i);

    // ---- Mesh-surface sample (mirrors SampleMeshSurface(id)). ----
    float2 a = PlacementRandom02(id, PLACEMENT_CH_POSITION);
    float2 b = PlacementRandom02(id, PLACEMENT_CH_ROTATION);
    float3 nrm;
    uint tri;
    float3 p = PlacementSampleMesh(pc.prm.Get(), a.x, a.y, b.x, b.y, nrm, tri);

    // ---- Density + accept. ----
    float q = PlacementScatterDensity(pc.prm.Get(), p, pc.seedLo, pc.seedHi);
    bool keep = (q >= 1.0f) || (PlacementRandom01(id, PLACEMENT_CH_DENSITY) < q);
    if (!keep)
        return;

    // ---- Prototype + transform (own channels). ----
    float rv = PlacementRandom01(id, PLACEMENT_CH_VARIANT);
    uint proto = PlacementScatterProto(rv, pc.prm.Get());
    float yaw = (PlacementRandom01(id, PLACEMENT_CH_ROTATION) * 2.0f - 1.0f) * 3.14159265f;
    float sc = pc.prm.Get().minScale
        + PlacementRandom01(id, PLACEMENT_CH_SCALE)
        * (pc.prm.Get().maxScale - pc.prm.Get().minScale);

    PlacementRenderInstance inst;
    inst.position = p;
    inst.yaw = yaw;
    inst.scale = float3(sc, sc, sc);
    inst.prototypeId = proto;
    inst.stableLow = id.x;
    inst.customB = i;

    uint slot;
    InterlockedAdd(pc.outCount.Get().count, 1u, slot);
    pc.outInstances.Get().items[slot] = inst;
}
