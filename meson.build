# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kyosuke Nekoyashiki

project('nekobench', 'c',
  version : '1.0.0',
  license : 'GPL-3.0-or-later',
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

# Bandwidth kernels: the program is built for the baseline ISA (or
# -march=native), and on x86 the kernels are also built for wider SIMD
# levels; the widest one the CPU supports is picked at run time.
kernel_libs = []
kvariants = []
if host_machine.cpu_family() == 'x86'
  kvariants += [['sse2', 'NB_REQ_SSE2', ['-msse2'], 'NB_BW_HAVE_SSE2']]
endif
if host_machine.cpu_family() in ['x86', 'x86_64']
  kvariants += [
    ['avx', 'NB_REQ_AVX', ['-mavx'], 'NB_BW_HAVE_AVX'],
    ['avx512', 'NB_REQ_AVX512', ['-mavx512f'], 'NB_BW_HAVE_AVX512'],
  ]
endif
foreach v : kvariants
  if cc.has_multi_arguments(v[2])
    kernel_libs += static_library('bwk_' + v[0], 'src/bw_kernels.c',
      c_args : c_args + v[2] + ['-DNB_KVAR=nb_bwk_' + v[0], '-DNB_KREQ=' + v[1]])
    c_args += '-D' + v[3]
  endif
endforeach

src = files(
  'src/main.c',
  'src/util.c',
  'src/topology.c',
  'src/latency.c',
  'src/bandwidth.c',
  'src/bw_kernels.c',
  'src/c2c.c',
  'src/html.c',
)

executable('nekobench', src, report_assets,
  c_args : c_args,
  link_with : kernel_libs,
  dependencies : deps,
  install : true)
