threads = dependency('threads')

axio_smoke_test = executable(
  'axio-smoke-test',
  'smoke/axio_smoke_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  dependencies: threads,
  include_directories: include_directories('../src'),
  install: false,
)

test('axio-smoke', axio_smoke_test)
