project('kms-quads',
  'c',
  default_options: [
    'warning_level=2',
    'c_std=gnu99',
  ]
)

cc = meson.get_compiler('c')
add_project_arguments(
  cc.get_supported_arguments([
    '-D_GNU_SOURCE',
    '-Wno-unused',
  ]),
  language: 'c'
)

defines = [ '-D_GNU_SOURCE' ]
if get_option('buildtype') == 'debug' or get_option('buildtype') == 'debugoptimized'
  defines += '-DDEBUG'
endif

deps = [
  dependency('libdrm'),
  dependency('gbm'),
  dependency('egl'),
]

if get_option('glcore')
  deps += dependency('gl')
  defines += '-DGL_GLEXT_PROTOTYPES=1'
  defines += '-DHAVE_GL_CORE=1'
else
  deps += dependency('glesv2')
endif

executable('kms-quads',
  'main.c', 'buffer.c', 'device.c', 'edid.c', 'egl-gles.c', 'kms.c',
  dependencies: deps,
  c_args: defines,
)
