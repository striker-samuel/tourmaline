// bindings_shared.h

#ifdef __HLSL_VERSION__   // defined by most HLSL compilers
#define CBUFFER(name, reg) cbuffer name : register(b##reg)
#define TEXTURE2D(name, reg) Texture2D name : register(t##reg)
#else
#define CBUFFER(name, reg) struct name
#define TEXTURE2D(name, reg) /* nothing in C++ */
#endif

// Shared binding slots
#define BINDING_ALBEDO   0
#define BINDING_NORMAL   1

CBUFFER(CameraData, 0)
{
    float4x4 viewProj;
    float3 cameraPos;
};

TEXTURE2D(Albedo, BINDING_ALBEDO);
TEXTURE2D(Normal, BINDING_NORMAL);
