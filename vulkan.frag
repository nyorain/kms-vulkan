#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = vec4(uv.x, uv.y, 1 - uv.x * uv.y, 1);
}

