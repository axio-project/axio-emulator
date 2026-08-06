threads = dependency('threads')
python3 = find_program('python3')

axio_smoke_test = executable(
  'axio-smoke-test',
  [
    'smoke/axio_smoke_test.cc',
    '../src/config.cc',
    '../src/dispatcher_impl/ethhdr.cc',
    '../src/dispatcher_impl/iphdr.cc',
  ],
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  dependencies: threads,
  include_directories: include_directories('../src', '../include'),
  install: false,
)

test('axio-smoke', axio_smoke_test, args: [meson.project_source_root()])

axio_config_contract_test = executable(
  'axio-config-contract-test',
  'config/config_contract_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  dependencies: axio_config_dep,
  install: false,
)

test(
  'axio-config-contract',
  axio_config_contract_test,
  args: [meson.project_source_root()],
)

test(
  'axio-configure-cli',
  python3,
  args: [files('config/axio_configure_cli_test.py'), axio_configure,
         meson.project_source_root()],
)

axio_runtime_config_test = executable(
  'axio-runtime-config-test',
  [
    'config/runtime_config_test.cc',
    '../src/config.cc',
  ],
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  dependencies: axio_config_dep,
  include_directories: include_directories('../src'),
  install: false,
)

test(
  'axio-runtime-config',
  axio_runtime_config_test,
  args: [meson.project_source_root()],
)

axio_build_config_fallback_test = executable(
  'axio-build-config-fallback-test',
  'config/build_config_override_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  include_directories: include_directories('../src'),
  install: false,
)

test('axio-build-config-fallback', axio_build_config_fallback_test)

axio_build_config_override_test = executable(
  'axio-build-config-override-test',
  'config/build_config_override_test.cc',
  cpp_args: [
    '--std=c++17',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-DAXIO_EXPECT_OVERRIDE=1',
    '-include',
    join_paths(meson.current_source_dir(), 'config/build_config_override.h'),
  ],
  include_directories: include_directories('../src'),
  install: false,
)

test('axio-build-config-override', axio_build_config_override_test)

test(
  'axio-identity',
  python3,
  args: [files('identity/axio_identity_guard.py'), meson.project_source_root()],
)

test(
  'axio-style',
  python3,
  args: [files('style/axio_style_guard.py'), meson.project_source_root()],
)
