#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec2 outUv;
void main() {
    const vec2 positions[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    const vec2 uvs[4] = vec2[4](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    outUv = uvs[gl_VertexIndex];
}
