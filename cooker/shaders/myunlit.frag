#version 450
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 outColor;
layout(set=0, binding=1) uniform sampler samp;
layout(set=2, binding=0) uniform MatParams { vec4 tint; } mp;
layout(set=2, binding=1) uniform texture2D baseColor;
void main() { outColor = texture(sampler2D(baseColor, samp), vUv) * mp.tint; }
