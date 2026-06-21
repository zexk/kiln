#version 450

/* No vertex buffer. Generates a fullscreen triangle from gl_VertexIndex. */
layout(location = 0) out vec2 frag_uv;

void main() {
    /* Vertices: (0,0), (2,0), (0,2) in UV space → covers the whole screen. */
    vec2 uv  = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    frag_uv = uv;
}
