// Clear-triangle fragment stage for unsigned-integer colour attachments. The
// host converts NVN's float clear values to the attachment's integer type and
// passes the raw bits in the same push-constant block.
#version 450
layout(push_constant) uniform P { uvec4 color; float depth; } pc;
layout(location = 0) out uvec4 o;
void main() { o = pc.color; }
