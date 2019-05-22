#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

void main() {
	// NOTE: we interpolate in non-linear srgb-like color space (which is
	// *incorrect* for most purposes but it looks linear in this case, we
	// aren't trying to achieve anything physcially). So we need to bring the
	// color into linear color space afterwards since that's what shaders
	// should output (e.g. for blending). That's what the pow is for
	fragColor = vec4(pow(vec3(uv.x, uv.y, 1 - uv.x * uv.y), vec3(2.2)), 1);

	// simple white-black color ramp
	// float gray = pow(uv.x, 2.2 * 2.2);
	// fragColor = vec4(gray, gray, gray, 1);
}

