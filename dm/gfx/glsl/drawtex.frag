// nvnCommandBufferDrawTexture's fragment stage: the source region of a texture,
// sampled through the handle's sampler, into the destination rectangle.
// gl_FragCoord is the destination pixel centre, as the Metal host's [[position]]
// is; dst.zw is source texels per destination pixel, so a swapped destination
// rectangle mirrors through its negative scale. src.zw is the source's size at
// its base level, which normalises the texel coordinate.
#version 450
layout(push_constant) uniform P {
    vec4 dst;   // dst.xy: destination origin in target pixels; dst.zw: texels per pixel
    vec4 src;   // src.xy: source origin in texels; src.zw: source size in texels
} pc;
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 o;
void main() {
    vec2 t = pc.src.xy + (gl_FragCoord.xy - pc.dst.xy) * pc.dst.zw;
    o = texture(tex, t / pc.src.zw);
}
