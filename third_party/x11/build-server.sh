# shellcheck shell=bash
# build-server.sh -- stage `server` (lane xserver), sourced by build.sh.
#
# Produces XHOBBY.BIN: X.Org xserver 21.1.x (dix/mi/fb + Render, Composite,
# Damage, XFixes, RandR, XKB, ...) with the HobbyOS DDX hw/hobbyos (source
# kept in third_party/x11/ddx/hobbyos, copied into the extracted tree), plus
# the host-compiled keymap disk/X11/US.XKM.  Deps (static, shared prefix):
#   pixman      plain C (no SIMD fast paths, no runtime CPU probing)
#   libfontenc, libXfont2 (built-in "fixed"/"cursor" fonts; PCF only)
#   libxkbfile
#   libsha1     tiny local SHA-1 (ddx/libsha1) for os/xsha1.c
# Requires the clientlibs stage in the same prefix (xorgproto, xtrans,
# libXau, libX11 for xkbfile).

_sv_pixman() {
  hb_meson pixman pixman-0.46.4.tar.xz --salt "$SV_SALT" -- \
    -Dloongson-mmi=disabled -Dmmx=disabled -Dsse2=disabled -Dssse3=disabled \
    -Dvmx=disabled -Darm-simd=disabled -Dneon=disabled -Da64-neon=disabled \
    -Dmips-dspr2=disabled -Drvv=disabled -Dgnu-inline-asm=disabled \
    -Dtls=disabled -Dopenmp=disabled -Dtimers=false -Dgnuplot=false \
    -Dgtk=disabled -Dlibpng=disabled -Dtests=disabled -Ddemos=disabled
}

_sv_fontenc() {
  hb_autotools libfontenc libfontenc-1.1.9.tar.xz --salt "$SV_SALT" -- \
    --with-encodingsdir=/X11/ENCODINGS
}

_sv_xfont2() {
  # Server-side core fonts: only the compiled-in builtins ("fixed",
  # "cursor") are used; no font server (fc), no FreeType rasteriser.
  # hypot(): not in the repo libc; supplied by ddx/compat (hbxcompat.a)
  # at the XHOBBY.BIN link, so the configure probe is answered here.
  # Library only (--make): its test programs would need hbxcompat.a.
  hb_autotools libXfont2 libXfont2-2.0.9.tar.xz --salt "$SV_SALT" --make libXfont2.la \
    --install "install-libLTLIBRARIES install-pkgconfigDATA install-libXfontincludeHEADERS" \
    -- ac_cv_lib_m_hypot=yes \
    --disable-devel-docs --without-xmlto --without-fop \
    --disable-freetype --disable-fc --enable-builtins --enable-pcfformat \
    --disable-bdfformat --disable-snfformat --without-bzip2 \
    --disable-ipv6 --disable-tcp-transport --enable-unix-transport --disable-local-transport
}

_sv_xkbfile() { hb_meson libxkbfile libxkbfile-1.2.0.tar.xz --salt "$SV_SALT"; }

# libsha1: compiled straight from ddx/libsha1 into the prefix (+ .pc so
# the xserver's meson -Dsha1=libsha1 finds it).
_sv_sha1() {
  local pkg=libsha1
  hb_want $pkg || return 0
  local key; key="$(cat "$HB_X11_DIR/ddx/libsha1/"* | sha256sum | cut -c1-16) $HB_ENV_KEY"
  key="$(printf '%s' "$key" | sha256sum | cut -c1-32)"
  if hb_done $pkg "$key"; then hb_log "$pkg: cached"; return 0; fi
  (
    hb_pkg_lock $pkg
    hb_done $pkg "$key" && exit 0
    hb_log "$pkg: building"; : > "$HB_LOG_DIR/$pkg.log"
    local o="$HB_OBJ_DIR/$pkg"; rm -rf "$o"; mkdir -p "$o" "$HB_PREFIX/lib/pkgconfig" "$HB_PREFIX/include"
    hb_run $pkg "$CC" $CFLAGS -c "$HB_X11_DIR/ddx/libsha1/libsha1.c" -o "$o/libsha1.o"
    rm -f "$HB_PREFIX/lib/libsha1.a"
    hb_run $pkg "$AR" rcs "$HB_PREFIX/lib/libsha1.a" "$o/libsha1.o"
    cp "$HB_X11_DIR/ddx/libsha1/libsha1.h" "$HB_PREFIX/include/"
    cat > "$HB_PREFIX/lib/pkgconfig/libsha1.pc" <<EOF
prefix=$HB_PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libsha1
Description: HobbyOS minimal SHA-1 (third_party/x11/ddx/libsha1)
Version: 0.3
Libs: -L\${libdir} -lsha1
Cflags: -I\${includedir}
EOF
    hb_mark $pkg "$key"; hb_log "$pkg: ok"
  )
}

# Server-stage environment: the ddx/include header overlay goes on -I (ahead
# of the wrappers' -isystem sysroot) and its hash salts every package key.
_sv_env() {
  SV_SALT="sv=$(cd "$HB_X11_DIR/ddx" && find include -type f | LC_ALL=C sort | xargs cat | sha256sum | cut -c1-16)"
  export CPPFLAGS="-I$HB_X11_DIR/ddx/include $CPPFLAGS"
}

# The X server itself.  Not installed into the (shared) prefix: the only
# product is build/<arch>/bin/XHOBBY.BIN (+ build/<arch>/sym/XHOBBY.elf with
# symbols), cached by a stamp in the per-lane build dir.
SV_XSERVER_TB=xorg-server-21.1.25.tar.xz
# listen_local=true is load-bearing: without LOCALCONN and abstract sockets
# (both Linux/SVR4-only) xtrans makes "unix" an alias and the real
# /tmp/.X11-unix/X<n> listener is the transport named "local".
_sv_xserver_meson_args() {
  local c_args="'-D__hobbyos__=1','-O2','-g','-fno-common','-I$HB_X11_DIR/ddx/include','-I$HB_PREFIX/include'"
  printf '%s\n' \
    -Dc_args="[$c_args]" \
    -Dhobbyos=true -Dhobbyos_link_args="['$HB_OBJ_DIR/xserver-compat/libhbxcompat.a']" \
    -Dxorg=false -Dxephyr=false -Dglamor=false -Dxnest=false -Dxvfb=false -Dxwin=false \
    -Dxquartz=false -Dglx=false -Dxdmcp=false -Dxdm-auth-1=false -Dsecure-rpc=false \
    -Dipv6=false -Dinput_thread=false \
    -Dxkb_dir=/X11/XKB -Dxkb_output_dir=/tmp -Dxkb_bin_dir=/X11 \
    -Ddefault_font_path=built-ins \
    -Dlisten_tcp=false -Dlisten_unix=true -Dlisten_local=true \
    -Dint10=false -Dsuid_wrapper=false -Dpciaccess=false -Dudev=false -Dudev_kms=false \
    -Dhal=false -Dsystemd_logind=false -Dvgahw=false -Ddpms=true -Dxf86bigfont=false \
    -Dscreensaver=true -Dxres=true -Dxace=true -Dxselinux=false -Dxinerama=true \
    -Dxcsecurity=false -Dxv=false -Dxvmc=false -Ddga=false -Dlinux_apm=false \
    -Dlinux_acpi=false -Dmitshm=false -Dagp=false -Dsha1=libsha1 \
    -Dxf86-input-inputtest=false -Ddri1=false -Ddri2=false -Ddri3=false -Ddrm=false \
    -Dlibunwind=false -Ddocs=false -Ddevel-docs=false -Ddocs-pdf=false -Ddtrace=false
}

_sv_xserver() {
  local pkg=xserver
  hb_want $pkg || { hb_log "$pkg: skipped (--only)"; return 0; }
  local -a margs; mapfile -t margs < <(_sv_xserver_meson_args)
  local ddxh key stamp="$HB_BUILD/.stamp-xserver" out="$HB_BIN_DIR/XHOBBY.BIN"
  ddxh="$(cd "$HB_X11_DIR/ddx" && find hobbyos compat -type f | LC_ALL=C sort | xargs cat | sha256sum | cut -c1-16)"
  # Deps' stamps fold in, so a rebuilt pixman/xfont2/... relinks the server.
  key="$(hb_key xserver "$SV_XSERVER_TB" "$SV_SALT ddx=$ddxh $(cat "$HB_PREFIX/.stamps/"{pixman,libfontenc,libXfont2,libxkbfile,libsha1,libX11,libxcb} 2>/dev/null | sha256sum | cut -c1-16) ${margs[*]}")"
  if [ "$HB_FORCE" != 1 ] && [ -f "$out" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$key" ] &&
     { [ -z "$HB_ONLY" ] || ! hb_want $pkg; }; then
    hb_log "$pkg: cached ($out)"; return 0
  fi
  hb_log "$pkg: building XHOBBY.BIN (meson)"
  : > "$HB_LOG_DIR/$pkg.log"
  local src b c; src="$(hb_src $pkg "$SV_XSERVER_TB")"; b="$HB_OBJ_DIR/$pkg"; c="$HB_OBJ_DIR/xserver-compat"
  rm -rf "$src/hw/hobbyos"; cp -r "$HB_X11_DIR/ddx/hobbyos" "$src/hw/hobbyos"
  # Server-private libc gap fillers (weak; see ddx/compat/hbxcompat.c).
  rm -rf "$c"; mkdir -p "$c"
  hb_run $pkg "$CC" $CFLAGS -I"$HB_X11_DIR/ddx/include" -c "$HB_X11_DIR/ddx/compat/hbxcompat.c" -o "$c/hbxcompat.o"
  hb_run $pkg "$AR" rcs "$c/libhbxcompat.a" "$c/hbxcompat.o"
  rm -rf "$b"
  hb_run $pkg meson setup "$b" "$src" \
    --cross-file "$HB_TOOLS/meson-paths.ini" --cross-file "$HB_X11_DIR/cross/meson-$HB_ARCH.ini" \
    --prefix /X11 --libdir lib --buildtype release -Ddefault_library=static "${margs[@]}"
  hb_run $pkg ninja -C "$b" -j"$HB_JOBS" hw/hobbyos/Xhobby
  mkdir -p "$HB_BUILD/sym"
  cp "$b/hw/hobbyos/Xhobby" "$HB_BUILD/sym/XHOBBY.elf"
  llvm-objcopy -O binary "$b/hw/hobbyos/Xhobby" "$out.tmp" && mv "$out.tmp" "$out"
  # The flat loader enters at the image base: _start must be there.
  local base start
  base="$(llvm-readelf -l "$HB_BUILD/sym/XHOBBY.elf" | awk '$1=="LOAD"{print $3; exit}')"
  start="$(llvm-nm "$HB_BUILD/sym/XHOBBY.elf" | awk '$3=="_start"{print "0x"$1; exit}')"
  [ "$((base))" = "$((start))" ] || hb_die "$pkg: _start ($start) is not at the image base ($base)"
  printf '%s\n' "$key" > "$stamp"
  hb_log "$pkg: ok ($out, $(stat -c %s "$out") bytes)"
}

# disk/X11/US.XKM: the evdev/pc105/us keymap compiled on the HOST (the
# server loads it instead of exec'ing xkbcomp).  Needs host xkbcomp and
# xkeyboard-config (Debian/Ubuntu: x11-xkb-utils, xkb-data).
_sv_xkm() {
  hb_want xkm || return 0
  local out="$HB_DISK_DIR/X11/US.XKM" xkbdir="${XKB_CONFIG_ROOT:-/usr/share/X11/xkb}"
  command -v xkbcomp >/dev/null || hb_die "xkm: host xkbcomp missing (apt install x11-xkb-utils xkb-data)"
  [ -d "$xkbdir/symbols" ] || hb_die "xkm: no xkeyboard-config at $xkbdir (set XKB_CONFIG_ROOT)"
  mkdir -p "$HB_DISK_DIR/X11"
  hb_run xkm xkbcomp -xkm -w 0 -I"$xkbdir" "$HB_X11_DIR/ddx/keymap/us.xkb" "$out.tmp"
  mv "$out.tmp" "$out"
  hb_log "xkm: $out ($(stat -c %s "$out") bytes)"
}

stage_server() {
  _sv_env
  _sv_pixman
  _sv_fontenc
  _sv_xfont2
  _sv_xkbfile
  _sv_sha1
  _sv_xserver
  _sv_xkm
}
