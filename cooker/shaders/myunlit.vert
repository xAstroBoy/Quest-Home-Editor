#version 450
#extension GL_EXT_multiview : enable
layout(location=0) in vec3 inPos;
layout(location=1) in vec2 inUv;
layout(set=0, binding=0) uniform GlobalUniforms { mat4 clipFromWorld0; mat4 clipFromWorld1; } g;
layout(push_constant) uniform Push { mat4 worldFromModel; } pc;
layout(location=0) out vec2 vUv;
void main() {
    mat4 cfw = (gl_ViewIndex == 0) ? g.clipFromWorld0 : g.clipFromWorld1;
    gl_Position = cfw * vec4(inPos, 1.0);
    vUv = inUv;
}
