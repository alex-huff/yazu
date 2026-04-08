project('yazu', 'c')

srcs = ['main.c', 'buffer.c']
incdir = include_directories('include')

wayland_client = dependency('wayland-client')
cairo = dependency('cairo')
deps = [wayland_client, cairo]

subdir('protocols')

executable(
	'yazu',
	sources : [srcs, protocols_srcs],
	dependencies : deps,
	include_directories : incdir,
	install : true,
)
