threads = dependency('threads')
python3 = find_program('python3')

axio_smoke_test = executable(
  'axio-smoke-test',
  [
    'smoke/axio_smoke_test.cc',
    '../src/dispatcher_impl/ethhdr.cc',
    '../src/dispatcher_impl/iphdr.cc',
  ],
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  dependencies: threads,
  include_directories: include_directories('../src', '../include'),
  install: false,
)

test('axio-smoke', axio_smoke_test)

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

axio_backend_capabilities_test = executable(
  'axio-backend-capabilities-test',
  'config/backend_capabilities_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  dependencies: axio_config_dep,
  install: false,
)

test(
  'axio-backend-capabilities',
  axio_backend_capabilities_test,
  args: [meson.project_source_root()],
)

axio_datapath_batching_test = executable(
  'axio-datapath-batching-test',
  'config/datapath_batching_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  include_directories: include_directories('../include'),
  install: false,
)

test('axio-datapath-batching', axio_datapath_batching_test)

axio_rx_completion_window_test = executable(
  'axio-rx-completion-window-test',
  'metrics/rx_completion_window_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  include_directories: include_directories('../src', '../include'),
  install: false,
)

test('axio-rx-completion', axio_rx_completion_window_test)

huge_alloc_test_link_args = []
if host_machine.system() == 'linux'
  huge_alloc_test_link_args += ['-lnuma']
endif

axio_huge_alloc_concurrency_test = executable(
  'axio-huge-alloc-concurrency-test',
  [
    'roce/huge_alloc_concurrency_test.cc',
    '../src/dispatcher_impl/roce/huge_alloc.cc',
  ],
  cpp_args: [
    '--std=c++17',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wno-unused-function',
  ],
  link_args: huge_alloc_test_link_args,
  dependencies: threads,
  include_directories: include_directories('../src', '../include'),
  install: false,
)

test('axio-huge-alloc-concurrency', axio_huge_alloc_concurrency_test)

axio_roce_buffer_pool_test = executable(
  'axio-roce-buffer-pool-test',
  [
    'roce/roce_buffer_pool_test.cc',
    '../src/dispatcher_impl/roce/huge_alloc.cc',
    '../src/dispatcher_impl/roce/roce_buffer_pool.cc',
  ],
  cpp_args: [
    '--std=c++17',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wno-unused-function',
  ],
  link_args: huge_alloc_test_link_args,
  dependencies: threads,
  include_directories: include_directories('../src', '../include'),
  install: false,
)

test('axio-roce-buffer-pool', axio_roce_buffer_pool_test)

test(
  'axio-roce-cleanup-contract',
  python3,
  args: [files('roce/roce_cleanup_contract_test.py'),
         meson.project_source_root()],
)

axio_metrics_record_test = executable(
  'axio-metrics-record-test',
  'metrics/metrics_record_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  include_directories: include_directories('../src', '../include'),
  install: false,
)

test('axio-metrics-record', axio_metrics_record_test)

axio_metrics_writer_test = executable(
  'axio-metrics-writer-test',
  [
    'metrics/metrics_writer_test.cc',
    '../src/metrics/metrics_writer.cc',
  ],
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  include_directories: include_directories('../src', '../include'),
  install: false,
)

test('axio-metrics-writer', axio_metrics_writer_test)

test(
  'axio-no-legacy-metrics-parser',
  python3,
  args: [files('metrics/no_legacy_parser_test.py'),
         meson.project_source_root()],
)

test(
  'axio-metrics-schema',
  python3,
  args: [
    files('metrics/metrics_schema_test.py'),
    files('metrics/fixtures/valid-window.jsonl'),
    files('metrics/fixtures/null-nic-window.jsonl'),
  ],
)

test(
  'axio-nic-rx-completion-analysis',
  python3,
  args: [files('metrics/analyze_completion_interval_test.py')],
)

test(
  'axio-readme-metrics',
  python3,
  args: [files('metrics/readme_metrics_test.py'), meson.project_source_root()],
)

test(
  'axio-metrics-cli',
  python3,
  args: [files('metrics/axio_metrics_cli_test.py')],
)

axio_effective_config_fingerprint_test = executable(
  'axio-effective-config-fingerprint-test',
  'config/effective_config_fingerprint_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  include_directories: include_directories('../include'),
  install: false,
)

test('axio-effective-config-fingerprint',
     axio_effective_config_fingerprint_test)

test(
  'axio-configure-cli',
  python3,
  args: [files('config/axio_configure_cli_test.py'), axio_configure.full_path(),
         meson.project_source_root()],
)

test(
  'axio-build-driver',
  python3,
  args: [files('config/axio_build_test.py'), meson.project_source_root()],
)

test(
  'axio-backend-build-matrix',
  python3,
  args: [files('config/backend_build_matrix_test.py'),
         axio_configure.full_path(), meson.project_source_root()],
)

test(
  'axio-toml-runtime-boundary',
  python3,
  args: [files('config/toml_runtime_guard.py'), meson.project_source_root()],
)

axio_runtime_options_test = executable(
  'axio-runtime-options-test',
  'config/runtime_options_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  dependencies: axio_config_dep,
  install: false,
)

test('axio-runtime-options', axio_runtime_options_test)

test(
  'axio-legacy-config-parser',
  python3,
  args: [files('config/legacy_config_parser_test.py'),
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
  include_directories: include_directories('../src', '../include'),
  install: false,
)

test(
  'axio-runtime-config',
  axio_runtime_config_test,
  args: [meson.project_source_root()],
)

axio_topology_contract_test = executable(
  'axio-topology-contract-test',
  'topology/topology_contract_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  dependencies: axio_config_dep,
  install: false,
)

test('axio-topology-contract', axio_topology_contract_test)

axio_topology_resource_pool_test = executable(
  'axio-topology-resource-pool-test',
  'topology/topology_resource_pool_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  dependencies: axio_config_dep,
  install: false,
)

test('axio-topology-resource-pool', axio_topology_resource_pool_test)

axio_build_config_fallback_test = executable(
  'axio-build-config-fallback-test',
  'config/build_config_override_test.cc',
  cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
  include_directories: include_directories('../src', '../include'),
  install: false,
)

test('axio-build-config-fallback', axio_build_config_fallback_test)

axio_build_config_roce_fallback_test = executable(
  'axio-build-config-roce-fallback-test',
  'config/build_config_override_test.cc',
  cpp_args: [
    '--std=c++17',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-DAXIO_CONFIG_NODE_TYPE=0',
    '-DAXIO_CONFIG_DPDK_MODE=0',
    '-DAXIO_CONFIG_ROCE_MODE=1',
    '-DAXIO_EXPECT_ROCE_FALLBACK=1',
  ],
  include_directories: include_directories('../src', '../include'),
  install: false,
)

test('axio-build-config-roce-fallback',
     axio_build_config_roce_fallback_test)

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
  include_directories: include_directories('../src', '../include'),
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
