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
]

if get_option('glcore')
  deps += dependency('gl')
  defines += '-DGL_GLEXT_PROTOTYPES=1'
  defines += '-DHAVE_GL_CORE=1'
else
  deps += dependency('glesv2')
endif

sources = files('main.c', 'buffer.c', 'device.c', 'edid.c', 'egl-gles.c', 'kms.c')

logind = dependency('lib' + get_option('logind-provider'), required: get_option('logind'), version: '>=237')

if logind.found()
    sources += files('logind.c')
    deps += logind
    defines += '-DHAVE_' + get_option('logind-provider').to_upper() + '=1'
    defines += '-DHAVE_LOGIND=1'
endif

libinput = dependency('libinput', required: get_option('input'))
libudev = dependency('libudev', required: get_option('input'))

if libinput.found() and libudev.found()
    sources += files('input.c')
    deps += libinput
    deps += libudev
    defines += '-DHAVE_INPUT=1'
endif

executable('kms-quads',
  sources,
  dependencies: deps,
  c_args: defines,
)
