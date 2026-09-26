// WYSIWYG compose vertex: fullscreen triangle (no vertex buffer —
// SV_VertexID, same shape as tonemap.vert.hlsl).
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut mainVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2(
        (id << 1) & 2,
        id & 2
    );
    o.pos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = uv;
    return o;
}
