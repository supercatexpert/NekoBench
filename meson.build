project('nekobench', 'c',
  version : '1.0.0',
  license : 'MIT',
  meson_version : '>= 0.60.0',
  default_options : [
    'c_std=gnu11',
    'buildtype=release',
    'warning_level=2',
    'b_ndebug=if-release',
  ])

cc = meson.get_compiler('c')

add_project_arguments('-D_GNU_SOURCE', language : 'c')

c_args = []
if get_option('native') and cc.has_argument('-march=native')
  c_args += '-march=native'
endif
# Keep the compiler from turning our store/copy loops into memset/memcpy calls.
c_args += cc.get_supported_arguments(['-fno-tree-loop-distribute-patterns'])

deps = [
  dependency('threads'),
  cc.find_library('m', required : false),
]

py = import('python').find_installation()
report_assets = custom_target('report_assets',
  input : ['src/assets/report.css', 'src/assets/report.js'],
  output : 'report_assets.h',
  command : [py, files('tools/embed.py'), '@OUTPUT@', '@INPUT@'])

src = files(
  'src/main.c',
  'src/util.c',
  'src/topology.c',
  'src/latency.c',
  'src/bandwidth.c',
  'src/c2c.c',
  'src/html.c',
)

executable('nekobench', src, report_assets,
  c_args : c_args,
  dependencies : deps,
  install : true)
