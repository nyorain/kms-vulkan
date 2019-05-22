#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
	float t; // in [0, 1], wrapping around
} ubo;

const float pi = 3.1415926535897932;

void main() {
	float t = 2 * pi * ubo.t;
	vec2 itp = 0.5 + mat2(cos(t), -sin(t), sin(t), cos(t)) * (0.5 - uv); 
	vec3 col = vec3(itp.x, itp.y, 1 - itp.x * itp.y);

	// NOTE: we interpolate in non-linear srgb-like color space (which is
	// *incorrect* for most purposes but it looks linear in this case, we
	// aren't trying to achieve anything physcially). So we need to bring the
	// color into linear color space afterwards since that's what shaders
	// should output (e.g. for blending). That's what the pow is for
	fragColor = vec4(pow(col, vec3(2.2)), 1);

	// simple white-black color ramp for testing
	// float gray = pow(uv.x, 2.2 * 2.2);
	// fragColor = vec4(gray, gray, gray, 1);
}

