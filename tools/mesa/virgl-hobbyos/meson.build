# HobbyOS virgl winsys (M2) -- the transport is /dev/gpu (docs/gpu-abi.md),
# not libdrm and not the vtest socket.  One static library, two TUs of
# interest: the winsys itself and the public entry point xlib.c calls.
libvirglhobbyos = static_library(
  'virglhobbyos',
  ['virgl_hobbyos_winsys.c'],
  include_directories : [
    inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_gallium_winsys,
    inc_gallium_drivers, inc_virtio,
  ],
  dependencies : [dep_libvirglcommon, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
