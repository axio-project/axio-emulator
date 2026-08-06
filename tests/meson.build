threads = dependency('threads')
python3 = find_program('python3')

axio_smoke_test = executable(
  'axio-smoke-test',
  ['smoke/axio_smoke_test.cc', '../src/config.cc'],
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  dependencies: threads,
  include_directories: include_directories('../src'),
  install: false,
)

test('axio-smoke', axio_smoke_test, args: [meson.project_source_root()])

test(
  'axio-identity',
  python3,
  args: [files('identity/axio_identity_guard.py'), meson.project_source_root()],
)
