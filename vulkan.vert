#version 450

layout(location = 0) out vec2 uv; // [0, 1]

void main() {
	// generate [-1, 1] quad for gl_Position
	uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(2 * uv - 1.0f, 0.0f, 1.0f);
}
