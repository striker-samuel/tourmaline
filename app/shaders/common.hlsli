#ifndef MAX_LIGHTS
#define MAX_LIGHTS 10
#endif


[[vk::push_constant]]
struct PC_Draw
{
    float4x4 model;
    uint materialIndex;
    float _pad[3];

    //Material
    float4 ambient;
    float4 diffuse;
    uint4 tex; //diffuse, ambient, spec
    //float4 _pad;
} pc;
