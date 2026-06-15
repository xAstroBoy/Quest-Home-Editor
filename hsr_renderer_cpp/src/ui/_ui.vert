#version 450
// Editor UI 2D vertex shader: pixel coords -> NDC via the invScreen push constant. (src for ui_shaders_spv.h)
layout(location=0) in vec2 inPos;
layout(location=1) in vec2 inUV;
layout(location=2) in vec4 inCol;
layout(push_constant) uniform PC { vec2 invScreen; } pc;
layout(location=0) out vec2 vUV;
layout(location=1) out vec4 vCol;
void main() {
    vUV = inUV;
    vCol = inCol;
    gl_Position = vec4(inPos.x * pc.invScreen.x - 1.0, inPos.y * pc.invScreen.y - 1.0, 0.0, 1.0);
}
