project(
    'benchmark_zlibs',
    'cpp',
    license: 'GPL-2.0-or-later',
    meson_version: '>= 1.3.0',
    default_options: [
        'cpp_std=c++17',
        'buildtype=release',
        'b_ndebug=if-release',
        'b_staticpic=false',
        'b_pie=false',
        'warning_level=3',
    ],
)

# zlib
zlib_dep = dependency('zlib')

# zlib-ng
cmake = import('cmake')
cmake_opts = cmake.subproject_options()
cmake_opts.add_cmake_defines(
    {
        'ZLIB_COMPAT': true,
        'WITH_OPTIM': true,
        'WITH_NATIVE_INSTRUCTIONS': true,
        'ZLIB_BUILD_STATIC': true,
        'BUILD_SHARED_LIBS': false,
        'PIC': false,
        'ZLIB_ENABLE_TESTS': false,
        'WITH_SANITIZER': false,
    },
)
zlib_ng_proj = cmake.subproject('zlib-ng', options: cmake_opts)
zlib_ng_dep = zlib_ng_proj.get_variable('zlib_dep')

executable(
    'benchmark_zlib',
    'src/main.cpp',
    dependencies: zlib_dep,
)

executable(
    'benchmark_zlib_ng',
    'src/main.cpp',
    dependencies: zlib_ng_dep,
)
