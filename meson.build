project('PIPETUNE', ['CPP'],
	# Get version number from file.
	version: run_command(find_program('cat'),
		files('./VERSION'), check: true).stdout().strip(),
)
fs = import('fs')

# get process environment
env = environment()

# find pkg-config
pkgconfig = find_program('pkg-config')

# project name
project_name = 'pipetune'
project_name_abbreviation = 'pipetune'

# compile flags
c_args = []

# load flags
ld_args = []

# ==================== Third-party Library Detection ====================
# indicator of whether necessary libraries were founded
has_dpdk = false
has_doca = false

# >>>>>>>>>>>>>> DPDK >>>>>>>>>>>>>>
root_dir = meson.current_source_dir()
if host_machine.cpu_family() == 'x86' or host_machine.cpu_family() == 'x86_64'
	dpdk_pc_path = root_dir + '/third_party/dpdk-stable-22.11.3/build/install/lib/x86_64-linux-gnu/pkgconfig'
	if not fs.is_dir(dpdk_pc_path)
		dpdk_pc_path = '/home/qijing/dpdk-stable-21.11.7/build/install/lib/x86_64-linux-gnu/pkgconfig'
	endif
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
	if dpdk_module_cflags.returncode() != 0 or dpdk_module_cflags.returncode() != 0 or dpdk_module_cflags.returncode() != 0
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

# >>>>>>>>>>>>>> DOCA >>>>>>>>>>>>>>
# doca_pc_path = '/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig/'
# doca_modules = ['doca-grpc-libs', 'doca-grpc', 'doca-libs', 'doca']
# founded_doca_modules = []
# message('>>> Detecting DOCA, assume pc file of DOCA is located at ' + doca_pc_path)
# env.set('PKG_CONFIG_PATH', doca_pc_path)
# foreach doca_module : doca_modules
# 	doca_module_cflags = run_command(pkgconfig, '--cflags', doca_module, env: env, check: false)
# 	doca_module_ldflags = run_command(pkgconfig, '--libs', '--static', doca_module, env: env, check: false) # add '--static' option if static link is needed
# 	doca_module_version = run_command(pkgconfig, '--modversion', doca_module, env: env, check: false)
# 	if doca_module_cflags.returncode() != 0 or doca_module_ldflags.returncode() != 0 or doca_module_version.returncode() != 0
# 		message('>>>>>> Failed to find ' + doca_module)
# 	else
# 		founded_doca_modules += doca_module
# 		c_args += doca_module_cflags.stdout().split()
# 		ld_args += doca_module_ldflags.stdout().split()
# 		message('>>>>>> Found ' + doca_module + ', version is ' + doca_module_version.stdout().split()[0])
# 	endif
# endforeach
# if founded_doca_modules.length() == doca_modules.length()
# 	has_doca = true
# 	add_global_arguments('-DHAS_DOCA', language: ['c', 'cpp'])
# endif

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


# ==================== Specified All Dependent Headers ==================
# scan all needed header files
glob = run_command('python3', files('./scan_include.py'), check: false)
inc_dirs = include_directories(glob.stdout().strip().split('\n'))
# message(glob.stdout().strip().split('\n'))
# =======================================================================


# ====================== Specifed All Source Files ======================
# scan all needed source files
glob = run_command('python3', files('./scan_src.py'), check: false)
sources = glob.stdout().strip().split('\n')
# message(sources)
# =======================================================================

# Add dependency of RhyR
RhyR_include = include_directories('/home/qijing/RhyR/include')
link_args = ['-L/home/qijing/RhyR/build', '-lRhyR', '-Wl,-rpath,/home/qijing/RhyR/build']
inc_dirs = [inc_dirs] + [RhyR_include]
ld_args += link_args

# Add dependency of hostCC
HostCC_include = include_directories('/home/qijing/RhyR/baseline')
link_args = ['-L/home/qijing/RhyR/baseline', '-lHostCC', '-Wl,-rpath,/home/qijing/RhyR/build']
inc_dirs = [inc_dirs] + [HostCC_include]
ld_args += link_args

# build executable
message('cflags: ' + ' '.join(c_args))
message('ldflags: ' + ' '.join(ld_args))
executable(project_name, sources,
	cpp_args: c_args,
	link_args: ld_args,
	include_directories: inc_dirs,
	install: false
)
