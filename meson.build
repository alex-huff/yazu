project('yazu', 'c')

srcs = ['main.c']
incdir = include_directories('include')

wayland_client = dependency('wayland-client')
deps = [wayland_client]

subdir('protocols')

executable(
	'yazu',
	sources : [srcs, protocols_srcs],
	dependencies : deps,
	include_directories : incdir,
	install : true,
)
