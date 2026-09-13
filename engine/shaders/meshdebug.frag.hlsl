// Meshlet debug view fragment: flat push color, no lighting/textures.
// C++ mirror: app::MeshDebugPush (144 bytes).
struct MeshDebugPush
{
    float4x4 model;
    float4x4 viewProj;
    float4 color;
};

[[vk::push_constant]]
ConstantBuffer<MeshDebugPush> pcD;

struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(PSIn IN)
{
     PSOut OUT;
     // Raw push color: the C++ side pre-scales it by 1/FrameExposure (see
     // the dbgGain/flatGain pre-scales in scene_renderer.cpp) so the
     // tonemapper's hdr * exposure reproduces the authored display color
     // (ACES still shapes it, hues stay distinct). A pow(1/2.2) used to
     // live here to survive an old tonemap with a gamma decode; today it
     // would just double-crush the palette.
     OUT.outColor = IN.color;
     return OUT;
}
