// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// rhitest_triangle.vert.hlsl - the RHI upload-barrier regression's draw
// consumer: a vertex-FREE triangle (SV_VertexID only, VertexLayoutId::Empty)
// whose shape is FIXED, so the only variable in the test's pixels is
// whether the indirect command delivered. Include-free on purpose (see
// rhitest_indirect_write.cs.hlsl).

struct RhiTestVsOut
{
    float4 pos : SV_Position;
};

RhiTestVsOut mainVS(uint vid : SV_VertexID)
{
    // A big fixed triangle over the viewport (NDC; symmetric in Y so no
    // clip-space flip reasoning is needed).
    const float2 verts[3] = {
        float2(-0.85f, -0.85f),
        float2(0.85f, -0.85f),
        float2(0.0f, 0.85f)
    };
    RhiTestVsOut o;
    const float2 p = verts[vid];
    o.pos = float4(p, 0.0f, 1.0f);
    return o;
}
