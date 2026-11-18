// Clear-triangle fragment stage for floating-point and normalized colour
// attachments. The pipeline's colour write mask selects the components the NVN
// clear asked for; a load-op pass keeps the others.
#version 450
layout(push_constant) uniform P { vec4 color; float depth; } pc;
layout(location = 0) out vec4 o;
void main() { o = pc.color; }
