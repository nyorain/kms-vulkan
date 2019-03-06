project('kms-skeleton',
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

executable('kms-skeleton',
  'main.c', 'buffer.c', 'device.c', 'edid.c', 'kms.c',
  dependencies: [ dependency('libdrm') ],
  c_args: defines,
)