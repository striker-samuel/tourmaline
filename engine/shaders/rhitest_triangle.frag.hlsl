// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// rhitest_triangle.frag.hlsl - the RHI upload-barrier regression's
// fragment half: flat white (see rhitest_triangle.vert.hlsl).

float4 mainPS() : SV_Target0
{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
