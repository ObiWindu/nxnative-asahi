// Full-target triangle for nvnCommandBufferDrawTexture: the destination
// rectangle is selected by the scissor and mapped in the fragment stage from
// gl_FragCoord, so the vertex stage only has to cover the render target.
#version 450
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
