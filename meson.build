project('yazu', 'c')

is_little_endian = host_machine.endian() == 'little'
project_arguments = ['-DYAZU_LITTLE_ENDIAN=@0@'.format(is_little_endian.to_int())]
add_project_arguments(project_arguments, language : 'c')

srcs = ['main.c', 'buffer.c']
incdir = include_directories('include')

egl = dependency('egl')
glesv2 = dependency('glesv2')
wayland_client = dependency('wayland-client')
wayland_egl = dependency('wayland-egl')
deps = [egl, glesv2, wayland_client, wayland_egl]

subdir('protocols')

executable(
	'yazu',
	sources : [srcs, protocols_srcs],
	dependencies : deps,
	include_directories : incdir,
	install : true,
)
