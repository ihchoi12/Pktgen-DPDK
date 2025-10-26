# Add C++ compiler support for PCM wrapper
add_languages('cpp', required: true)

# Import filesystem module
fs = import('fs')

# PCM library configuration for static linking
pcm_base_path = meson.current_source_dir() / '..' / '..' / 'pcm'
pcm_lib_path = pcm_base_path / 'build' / 'src'
pcm_inc_path = pcm_base_path / 'src'
common_pcm_path = meson.current_source_dir() / '..' / '..' / 'common' / 'pcm'

# Check for PCM static library
pcm_static_lib = pcm_lib_path / 'libpcm.a'
pcm_found = false

if fs.is_file(pcm_static_lib)
    pcm_found = true
    message('Found PCM static library: ' + pcm_static_lib)
endif

sources = files(
	'cli-functions.c',
    'l2p.c',
	'pktgen-arp.c',
	'pktgen-capture.c',
	'pktgen-cmds.c',
	'pktgen-cpu.c',
	'pktgen-display.c',
	'pktgen-dump.c',
	'pktgen-ether.c',
	'pktgen-gre.c',
	'pktgen-gtpu.c',
	'pktgen-ipv4.c',
	'pktgen-ipv6.c',
	'pktgen-latency.c',
	'pktgen-log.c',
	'pktgen-main.c',
	'pktgen-pcap.c',
	'pktgen-port-cfg.c',
	'pktgen-random.c',
	'pktgen-range.c',
	'pktgen-seq.c',
	'pktgen-stats.c',
	'pktgen-sys.c',
	'pktgen-tcp.c',
	'pktgen-udp.c',
	'pktgen-vlan.c',
	'pktgen-workq.c',
	'pktgen.c',
	'pktgen_pcm.c',
	'xorshift64star.c',
)

# Add common PCM wrapper if library found
if pcm_found
    sources += files('../../common/pcm/common_pcm_wrapper.cpp')
    message('PCM integration enabled with common wrapper')
endif

if get_option('enable_lua')
    sources += files('lpktgenlib.c')
endif

cflags = ['-D__PROJECT_VERSION="' + meson.project_version() + '"']

# Initialize ldflags and includes
ldflags = []
includes = []
cpp_args = []

# Add PCM support if available
if pcm_found
    # C files only need common wrapper header (not direct PCM headers which are C++)
    cflags += ['-I' + meson.current_source_dir() / '..' / '..' / 'common' / 'pcm']
    # C++ files need both PCM headers and common wrapper
    cpp_args += ['-I' + meson.current_source_dir() / '..' / '..' / 'pcm' / 'src',
                 '-I' + meson.current_source_dir() / '..' / '..' / 'common' / 'pcm']
    ldflags += [pcm_static_lib, '-static-libstdc++', '-static-libgcc', '-lpthread', '-ldl', '-lnuma']
endif

deps = [dpdk, common, utils, vec, plugin, cli, lua, hmap]

if fgen_dep.found()
    deps += [fgen_dep]
endif

deps += [cc.find_library('rte_net_i40e', dirs: [dpdk_libs_path], required: false)]
deps += [cc.find_library('rte_net_ixgbe', dirs: [dpdk_libs_path], required: false)]
deps += [cc.find_library('rte_net_ice', dirs: [dpdk_libs_path], required: false)]
deps += [cc.find_library('rte_bus_vdev', dirs: [dpdk_libs_path], required: false)]

deps += [dependency('threads')]
deps += [dependency('numa', required: false)]
deps += [dependency('pcap', required: true)]
deps += [cc.find_library('dl', required: false)]
deps += [cc.find_library('m', required: false)]
deps += [cc.find_library('bsd', required: true)]

pktgen = executable('pktgen',
		sources,
		c_args: cflags,
		cpp_args: cpp_args,
		link_args: ldflags,
		include_directories: includes,
		install: true,
		dependencies: [deps, lua_dep, dpdk_bond])
