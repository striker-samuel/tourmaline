#version 450
layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

//layout(binding=2) uniform sampler2D texSampler;
const int MAX_MATERIALS = 100;
layout(binding = 2) uniform sampler2D materials[MAX_MATERIALS];

layout(push_constant) uniform PushConsts {
    mat4 view;
    mat4 proj;
    int material_index;
	int     _pad0[3];
    //int here is size 32
} pc;


void main() {
    outColor = texture(materials[pc.material_index], fragTexCoord);
}