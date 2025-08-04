#version 450
// pull in the Vulkan-GLSL built-ins (gl_VertexIndex, gl_InstanceIndex, etc.)
#extension GL_KHR_vulkan_glsl : enable

// This should be a define symbol in cmake land. then pass it onto build glslc. and to cpp build :)
const int MAX_INSTANCES = 256;

layout(binding = 0) uniform VPBUFFER {
//    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(set=0, binding=1, std140) uniform InstanceData {
    mat4 model[MAX_INSTANCES];
} instance;


layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main() {
    gl_Position = ubo.proj * ubo.view * instance.model[gl_InstanceIndex] * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}

