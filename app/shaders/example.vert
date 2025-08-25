#version 450
#extension GL_KHR_vulkan_glsl : enable

// Override at compile time with: glslc -DMAX_INSTANCES=... ...
#ifndef MAX_INSTANCES
#define MAX_INSTANCES 1000
#endif

layout(push_constant) uniform PushConsts {
    mat4 view;
    mat4 proj;
    int material_index;
    int padding[3];
} pc;


layout(set = 0, binding = 0, std140) uniform IndexUBO {
    uint indices[MAX_INSTANCES];
} inn;



layout(set = 0, binding = 1, std140) buffer InstanceData {
    mat4 model[MAX_INSTANCES];
} instance;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main() {
    gl_Position = pc.proj * pc.view * instance.model[inn.indices[gl_InstanceIndex]] * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}
