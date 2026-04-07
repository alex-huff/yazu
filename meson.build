project('yazu', 'c')

srcs = ['main.c']
incdir = include_directories('include')

wayland_client = dependency('wayland-client')
deps = [wayland_client]

executable('yazu', sources : srcs, dependencies : deps, include_directories : incdir)
