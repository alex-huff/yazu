wayland_protocols = dependency('wayland-protocols')
wayland_scanner = dependency('wayland-scanner', native : true)

wayland_scanner_program = find_program(wayland_scanner.get_variable('wayland_scanner'), native : true)

wayland_protocols_dir = wayland_protocols.get_variable('pkgdatadir')

wayland_scanner_code = generator(
	wayland_scanner_program,
	output : '@BASENAME@-protocol.c',
	arguments : ['private-code', '@INPUT@', '@OUTPUT@'],
)
wayland_scanner_client = generator(
	wayland_scanner_program,
	output : '@BASENAME@-client-protocol.h',
	arguments : ['client-header', '@INPUT@', '@OUTPUT@'],
)

protocols = [
	wayland_protocols_dir / 'stable/xdg-shell/xdg-shell.xml',
	'wlr-layer-shell-unstable-v1.xml',
]
protocols_srcs = []
foreach xml_file : protocols
	protocols_srcs += wayland_scanner_code.process(xml_file)
	protocols_srcs += wayland_scanner_client.process(xml_file)
endforeach
