// Clear-triangle fragment stage for a depth/stencil-only pass: the triangle
// writes no colour. The vertex stage places it at the clear's depth and the
// fixed-function state writes depth (ALWAYS, enabled for a depth clear) and
// stencil (REPLACE) through the clear's masks.
#version 450
void main() {}
