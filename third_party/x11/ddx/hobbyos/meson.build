# hw/hobbyos -- HobbyOS framebuffer X server (XHOBBY.BIN).  Copied into the
# extracted xserver tree by third_party/x11/build-server.sh; enabled with
# -Dhobbyos=true (patches/xserver adds the option + subdir).
srcs = [
    'hbscreen.c',
    'hbinput.c',
    '../../mi/miinitext.c',
    '../../mi/miinitext.h',
]

hobbyos_server = executable(
    'Xhobby',
    srcs,
    include_directories: [inc, include_directories('../../miext/shadow')],
    dependencies: common_dep,
    link_with: [
        libxserver_main,
        libxserver_fb,
        libxserver,
        libxserver_miext_shadow,
        libxserver_xkb_stubs,
        libxserver_xi_stubs,
        libxserver_glx,
        libglxvnd,
    ],
    link_args: get_option('hobbyos_link_args'),
    install: false,
)
