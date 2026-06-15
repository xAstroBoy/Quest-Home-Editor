#version 450
// Editor UI 2D fragment shader. Colors are authored in sRGB; the swapchain is sRGB (it re-encodes on write),
// so convert to linear here or the dark Blender theme washes out bright. atlas.r = glyph coverage / white texel.
layout(location=0) in vec2 vUV;
layout(location=1) in vec4 vCol;
layout(set=0, binding=0) uniform sampler2D atlas;
layout(location=0) out vec4 outColor;
void main() {
    float cov = texture(atlas, vUV).r;
    vec3 lin = pow(max(vCol.rgb, vec3(0.0)), vec3(2.2));   // sRGB -> linear (the framebuffer re-encodes)
    outColor = vec4(lin, vCol.a * cov);
}
