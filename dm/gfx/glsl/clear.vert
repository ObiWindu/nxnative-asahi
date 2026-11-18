// Full-target triangle for a clear a Vulkan load action cannot express (colour
// channel masks, partial stencil-bit masks and their scissor rectangles).
// The vertex stage emits (-1,-1) (3,-1) (-1,3) and places it at the clear's
// depth: clip z == framebuffer z under Vulkan's [0, 1] depth range.
#version 450
layout(push_constant) uniform P { vec4 color; float depth; } pc;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, pc.depth, 1.0);
}
