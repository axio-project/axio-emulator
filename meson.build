project('AXIO', ['CPP'],
	# Get version number from file.
	version: run_command(find_program('cat'),
		files('./VERSION'), check: true).stdout().strip(),
)

subdir('third_party/tomlplusplus')

axio_config_include = include_directories('include')
axio_config_sources = [
	'src/config/backend_capabilities.cc',
	'src/config/config_loader.cc',
	'src/config/config_validator.cc',
	'src/config/legacy_converter.cc',
	'src/config/runtime_options.cc',
	'src/config/topology.cc',
	'src/config/topology_resource_pool.cc',
]
axio_config_cpp_args = ['--std=c++17', '-Wall', '-Wextra', '-Werror']
axio_config_library = static_library(
	'axio-config',
	axio_config_sources,
	cpp_args: axio_config_cpp_args,
	dependencies: tomlplusplus_dep,
	include_directories: axio_config_include,
	install: false,
)
axio_config_dep = declare_dependency(
	link_with: axio_config_library,
	dependencies: tomlplusplus_dep,
	include_directories: axio_config_include,
)

axio_config_native_library = static_library(
	'axio-config-native',
	axio_config_sources,
	native: true,
	cpp_args: axio_config_cpp_args,
	dependencies: tomlplusplus_dep,
	include_directories: axio_config_include,
	install: false,
)
axio_config_native_dep = declare_dependency(
	link_with: axio_config_native_library,
	dependencies: tomlplusplus_dep,
	include_directories: axio_config_include,
)

axio_configure = executable(
	'axio-configure',
	'toolchain/axio_configure.cc',
	native: true,
	cpp_args: ['--std=c++17', '-Wall', '-Wextra', '-Werror'],
	dependencies: axio_config_native_dep,
	install: false,
)

build_datapath = get_option('datapath')

if build_datapath
fs = import('fs')

axio_generated_config_sources = []
axio_generated_config_cpp_args = []

axio_build_metadata = configuration_data()
axio_build_metadata.set_quoted('AXIO_VERSION', meson.project_version())
git_program = find_program('git', required: false)
if git_program.found()
	axio_build_metadata.set_quoted('AXIO_GIT_COMMIT', '@VCS_TAG@')
else
	axio_build_metadata.set_quoted('AXIO_GIT_COMMIT', 'unknown')
endif
axio_build_metadata_template = configure_file(
	output: 'axio_build_metadata_generated.h.in',
	configuration: axio_build_metadata,
	output_format: 'c',
)
if git_program.found()
	axio_build_metadata_header = vcs_tag(
		input: axio_build_metadata_template,
		output: 'axio_build_metadata_generated.h',
		fallback: 'unknown',
		command: [
			'git',
			'-C', meson.current_source_dir(),
			'rev-parse', 'HEAD',
		],
		replace_string: '@VCS_TAG@',
	)
else
	axio_build_metadata_header = axio_build_metadata_template
endif
axio_generated_config_sources += [axio_build_metadata_header]
axio_generated_config_cpp_args += [
	'-include',
	axio_build_metadata_header.full_path(),
]

axio_config_path = get_option('axio_config')
if axio_config_path != ''
	if not axio_config_path.startswith('/')
		error('axio_config must be an absolute path: ' + axio_config_path)
	endif
	axio_config_input = files(axio_config_path)
	subdir('generated')
	axio_generated_config_sources += [axio_generated_config_header]
	axio_generated_config_cpp_args += [
		'-include',
		axio_generated_config_header.full_path(),
	]
endif

# get process environment
env = environment()

# find pkg-config
pkgconfig = find_program('pkg-config')

# project name
project_name = 'axio'

# compile flags
c_args = []

# load flags
ld_args = []

# ==================== Third-party Library Detection ====================
# indicator of whether necessary libraries were founded
has_dpdk = false

# >>>>>>>>>>>>>> DPDK >>>>>>>>>>>>>>
root_dir = meson.current_source_dir()
if host_machine.cpu_family() == 'x86' or host_machine.cpu_family() == 'x86_64'
	dpdk_pc_path = root_dir + '/third_party/dpdk-stable-22.11.3/build/install/lib/x86_64-linux-gnu/pkgconfig'
elif host_machine.cpu_family() == 'arm' or host_machine.cpu_family() == 'aarch64'
	dpdk_pc_path = '/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig'
endif

dpdk_modules = ['libdpdk', 'libdpdk-libs']
founded_dpdk_modules = []
env.set('PKG_CONFIG_PATH', dpdk_pc_path)
message('>>> Detecting DPDK, assume pc file of DPDK is located at ' + dpdk_pc_path)
foreach dpdk_module : dpdk_modules
	dpdk_module_cflags = run_command(pkgconfig, '--cflags', dpdk_module, env: env, check: false)
	 # TODO: when compile with dpdk, remove '--static' option
	dpdk_module_ldflags = run_command(pkgconfig, '--libs', '--static', dpdk_module, env: env, check: false)
	dpdk_module_version = run_command(pkgconfig, '--modversion', dpdk_module, env: env, check: false)
	if dpdk_module_cflags.returncode() != 0 or dpdk_module_ldflags.returncode() != 0 or dpdk_module_version.returncode() != 0
		message('>>> Failed to find ' + dpdk_module)
	else
		founded_dpdk_modules += dpdk_module
		c_args += dpdk_module_cflags.stdout().split()
		ld_args += dpdk_module_ldflags.stdout().split()
		message('>>>>>> Found ' +  dpdk_module + ', version is ' + dpdk_module_version.stdout().split()[0])
	endif
endforeach
if founded_dpdk_modules.length() == dpdk_modules.length()
	has_dpdk = true
endif

# >>>>>>>>>>>>>> OTHERS >>>>>>>>>>>>>>
ld_args += ['-lpthread', '-lrte_net_bond', '-lrte_bus_pci', '-lrte_bus_vdev', '-ldl', '-lnuma', '-libverbs']

## Note: we detect different DPDK and DOCA version inside meson and source files,
##		so we block all deprecated-declarations warnings here to make log output of 
##		the build process cleaner
c_args += ['-D_GNU_SOURCE', '-Wno-deprecated-declarations', '-Wno-unused-function']
c_args += ['-g', '-O3', '--std=c++17']

# necessary dependencies check
assert(has_dpdk, 'couldn\'t find libdpdk')
# =======================================================================


inc_dirs = include_directories('src')
sources = files(
	'src/config.cc',
	'src/dispatcher_impl/dispatcher.cc',
	'src/dispatcher_impl/dpdk/dpdk_dispatcher.cc',
	'src/dispatcher_impl/dpdk/dpdk_dispatcher_dataplane.cc',
	'src/dispatcher_impl/dpdk/dpdk_externs.cc',
	'src/dispatcher_impl/dpdk/dpdk_init.cc',
	'src/dispatcher_impl/dpdk/dpdk_pkt_handlers.cc',
	'src/dispatcher_impl/ethhdr.cc',
	'src/dispatcher_impl/iphdr.cc',
	'src/dispatcher_impl/roce/huge_alloc.cc',
	'src/dispatcher_impl/roce/roce_buffer_pool.cc',
	'src/dispatcher_impl/roce/roce_dispatcher.cc',
	'src/dispatcher_impl/roce/roce_dispatcher_dataplane.cc',
	'src/dispatcher_impl/roce/roce_pkt_handlers.cc',
	'src/main.cc',
	'src/metrics/metrics_writer.cc',
	'src/util/network_stats.cc',
	'src/util/numautils.cc',
	'src/ws_impl/msg_handlers.cc',
	'src/ws_impl/workspace.cc',
)


# build executable
executable(project_name, sources + axio_generated_config_sources,
	cpp_args: c_args + axio_generated_config_cpp_args,
	link_args: ld_args,
	dependencies: axio_config_dep,
	include_directories: inc_dirs,
	install: false
)
endif

subdir('tests')
