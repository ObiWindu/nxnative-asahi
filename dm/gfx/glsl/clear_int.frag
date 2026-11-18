// Clear-triangle fragment stage for signed-integer colour attachments.
#version 450
layout(push_constant) uniform P { ivec4 color; float depth; } pc;
layout(location = 0) out ivec4 o;
void main() { o = pc.color; }
