#version 450
// src/user/cp/vkcube.frag — flat per-vertex colour (vkcube.c).
layout(location = 0) in vec3 vcol;
layout(location = 0) out vec4 outc;
void main() { outc = vec4(vcol, 1.0); }
