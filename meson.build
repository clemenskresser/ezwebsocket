project('ezwebsocket', 'c',
    version : '2.1.0',
    meson_version: '>= 0.50.0',
    license : 'MIT',
    default_options : [ 'c_std=gnu11' ])

pkg = import('pkgconfig')

dep_threads = dependency('threads')

deps_libezwebsocket = [
    dependency('threads'),
]

inc_public = include_directories('inc')
inc_common = [ include_directories('.'), inc_public ]

subdir('inc')
subdir('src')

if get_option('examples').enabled()
  subdir('tests')
endif

