project('kms-quads',
  'c',
  default_options: [
    'warning_level=3',
    'c_std=c99',
  ]
)

cc = meson.get_compiler('c')
add_project_arguments(
  cc.get_supported_arguments([
    '-Wno-unused',
  ]),
  language: 'c'
)

defines = ['-D_POSIX_C_SOURCE=200809L']
if get_option('buildtype') == 'debug' or get_option('buildtype') == 'debugoptimized'
  defines += '-DDEBUG'
endif

deps = [
  dependency('libdrm'),
  dependency('gbm'),
  dependency('egl'),
  dependency('vulkan'),
]

if get_option('glcore')
  deps += dependency('gl')
  defines += '-DGL_GLEXT_PROTOTYPES=1'
  defines += '-DHAVE_GL_CORE=1'
else
  deps += dependency('glesv2')
endif

# generate vulkan shaders
shaders = []
glslang = find_program('glslangValidator')
shaders_src = [
	'vulkan.vert',
	'vulkan.frag',
]

foreach shader : shaders_src
	name = shader.underscorify() + '_data'
	args = [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', name]
	header = custom_target(
		shader + '_spv',
		output: shader + '.h',
		input: shader,
		command: args)
	shaders += [header]
endforeach

src = [
  'main.c',
  'buffer.c',
  'device.c',
  'edid.c',
  'egl-gles.c',
  'kms.c',
  'vulkan.c',
  shaders,
]
executable('kms-quads', src,
  dependencies: deps,
  c_args: defines,
)
