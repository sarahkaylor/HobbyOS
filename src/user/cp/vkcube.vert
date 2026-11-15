#version 450
// src/user/cp/vkcube.vert — the C5-vk spinning cube (vkcube.c).  The cube is
// generated from gl_VertexIndex: no vertex buffer, 36 vertices, 6 faces.
layout(push_constant) uniform PC { mat4 mvp; } pc;
layout(location = 0) out vec3 vcol;

const vec3 P[8] = vec3[8](vec3(-1, -1, -1), vec3(1, -1, -1), vec3(1, 1, -1), vec3(-1, 1, -1),
                          vec3(-1, -1, 1), vec3(1, -1, 1), vec3(1, 1, 1), vec3(-1, 1, 1));
const int I[36] = int[36](0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,  0, 1, 5, 0, 5, 4,
                          3, 7, 6, 3, 6, 2,  0, 4, 7, 0, 7, 3,  1, 2, 6, 1, 6, 5);
const vec3 C[6] = vec3[6](vec3(0.95, 0.25, 0.25), vec3(0.25, 0.9, 0.3), vec3(0.25, 0.45, 1.0),
                          vec3(1.0, 0.85, 0.2), vec3(0.9, 0.3, 0.9), vec3(0.2, 0.9, 0.9));

void main() {
  int face = gl_VertexIndex / 6;
  vec3 p = P[I[gl_VertexIndex]];
  gl_Position = pc.mvp * vec4(p, 1.0);
  // a little per-corner shading so the edges read in a screenshot
  vcol = C[face] * (0.70 + 0.30 * (0.5 + 0.25 * (p.x + p.y)));
}
