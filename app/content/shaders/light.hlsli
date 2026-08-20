#pragma once

struct Light
{
    float4 color; // 16B

    float3 position; // 12B
    int flags;
    //float _pad0; // 4B = pad to 16B

    float3 direction; // 12B
    float intensity; // 4B

    int type; // 4B
    float innerAngle;
    float outerAngle;
    int shadow_map_index;
    float4x4 lightVP;
};

struct SpotLight
{
    float4 color; // 16B

    float3 position; // 12B
    int flags;
    //float _pad0; // 4B = pad to 16B

    float3 direction; // 12B
    float intensity; // 4B

    int type; // 4B
    float innerAngle;
    float outerAngle;
    int shadow_map_index;
    float4x4 lightVP;
};



