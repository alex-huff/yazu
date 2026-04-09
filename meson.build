project('yazu', 'c')

is_little_endian = host_machine.endian() == 'little'
project_arguments = ['-DYAZU_LITTLE_ENDIAN=@0@'.format(is_little_endian.to_int())]
add_project_arguments(project_arguments, language : 'c')

srcs = ['main.c', 'buffer.c', 'pixfmt.c']
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
