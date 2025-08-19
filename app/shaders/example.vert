#version 450

// Override at compile time with: glslc -DMAX_INSTANCES=... ...
#ifndef MAX_INSTANCES
#define MAX_INSTANCES 256
#endif

layout(push_constant) uniform PC {
    mat4 view;
    mat4 proj;
} pc;


layout(set = 0, binding = 0) uniform VPBUFFER {
    mat4 view;
    mat4 proj;
} ubo;

layout(set = 0, binding = 1, std140) uniform InstanceData {
    mat4 model[MAX_INSTANCES];
} instance;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main() {
    gl_Position = pc.proj * pc.view * instance.model[gl_InstanceIndex] * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}
