# shellcheck shell=bash
# build-icewm.sh -- stage `icewm` (lane icewm), sourced by build.sh.
#
# M4 (icewm.md §3): IceWM 4.1.0 cross-built with CMake against the shared
# X11 prefix, linked as flat HobbyOS programs:
#   bin/ICEWM.BIN  bin/ICEWMBG.BIN  bin/ICESH.BIN  bin/ICEWMHNT.BIN
# plus the /ICEWM data tree (LIBDIR = CFGDIR = DATADIR = /ICEWM, §4.4) in
#   disk/ICEWM/{preferences,menu,toolbar,programs,keys,winoptions,theme,
#               themes/<default + 1>,icons,ledclock,mailbox,taskbar,...}
# and the "clients" sub-step: real-Xlib rebuilds of the in-tree shim demos
#   bin/XEYESR.BIN  bin/XCALCR.BIN   (src/user/x11/apps/{xeyes,xcalc}/main.c,
#                                     compiled unchanged against libX11)
#
# Configuration (icewm.md §1): XPM + libpng + libjpeg image loaders, Xft
# (+ core fonts fallback), Xrandr, Xinerama, shape; no imlib2 / gdk-pixbuf /
# SVG, no NLS / I18N, no session management, no sound, no fribidi, no XRes
# (built, but IceWM only uses it for the "kill client" pid lookup and its
# probe cannot run cross), no freedesktop menus (icewm-menu-fdo needs C++
# exceptions).  libc++ is -fno-exceptions -frtti like the repo's.
#
# HobbyOS adaptations live in patches/icewm/*.patch (sources byte-identical
# to the pinned tarball) and in icewm/ next to this file:
#   icewm/include/   lane-local POSIX headers the repo libc lacks
#                    (wordexp.h glob.h fnmatch.h ...), searched with -I so
#                    nothing outside this stage sees them
#   icewm/compat/    their implementations -> libhbicecompat.a
#   icewm/data/      HobbyOS preferences / menu / toolbar / programs / keys
#
# Program launching: HobbyOS has fork/execve/execvp (PATH defaults to "/")
# and /SH.BIN understands `-c`; the patches route IceWM's "/bin/sh -c" to
# "/SH.BIN -c" and name its own executables by their 8.3 .BIN names.

IW_VER=4.1.0
IW_TB=icewm-$IW_VER.tar.gz
IW_REV=r1   # bump to force a rebuild after recipe changes

_iw_flags() {
  # CXXFLAGS for the CMake build: the env.sh C++ flags plus our overlay.
  printf '%s' "$HB_CXXFLAGS -I$HB_X11_DIR/icewm/include -include $HB_X11_DIR/icewm/include/hb_icewm_compat.h -isystem $HB_REPO/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxxabi/include -DHOBBYOS_ICEWM=1"
}

# libhbicecompat.a: lane-local libc gap fillers (C, both arches).
_iw_compat() {
  local o="$HB_OBJ_DIR/icewm-compat"; rm -rf "$o"; mkdir -p "$o"
  local c
  for c in "$HB_X11_DIR"/icewm/compat/*.c "$HB_X11_DIR"/icewm/compat/*.cc; do
    [ -f "$c" ] || continue
    case "$c" in
      *.cc) hb_run icewm "$CXX" $CXXFLAGS -I"$HB_X11_DIR/icewm/include" -c "$c" -o "$o/$(basename "${c%.cc}").o" ;;
      *)    hb_run icewm "$CC" $CFLAGS -I"$HB_X11_DIR/icewm/include" -c "$c" -o "$o/$(basename "${c%.c}").o" ;;
    esac
  done
  rm -f "$HB_OBJ_DIR/libhbicecompat.a"
  hb_run icewm llvm-ar rcs "$HB_OBJ_DIR/libhbicecompat.a" "$o"/*.o
}

_iw_key() {
  local extra
  extra="$( (cd "$HB_X11_DIR/icewm" && find include compat -type f | LC_ALL=C sort | xargs cat) | sha256sum | cut -c1-16)"
  hb_need_libcxx
  hb_key icewm "$IW_TB" "$IW_REV $extra $(sha256sum "$HB_X11_DIR/build-icewm.sh" | cut -c1-16) rt=$(hb_runtime_key)"
}

_iw_build() {
  hb_want icewm || { hb_log "icewm: skipped (--only)"; return 0; }
  local key; key="$(_iw_key)"
  local stamp="$HB_BUILD/.icewm.stamp"
  if [ "$HB_FORCE" != 1 ] && [ -z "$HB_ONLY" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$key" ] \
     && [ -f "$HB_BIN_DIR/ICEWM.BIN" ] && [ -f "$HB_BIN_DIR/ICEWMBG.BIN" ]; then
    hb_log "icewm: cached"; return 0
  fi
  hb_log "icewm: building $IW_VER (cmake)"
  : > "$HB_LOG_DIR/icewm.log"
  hb_need_libcxx
  _iw_compat
  local src b; src="$(hb_src icewm "$IW_TB")"; b="$HB_OBJ_DIR/icewm"
  rm -rf "$b"
  local cxxflags; cxxflags="$(_iw_flags)"
  # The toolchain's try_compile builds static libraries, so link-time
  # CHECK_FUNCTION_EXISTS / CHECK_LIBRARY_EXISTS probes would all "pass":
  # answer them explicitly from what src/libc really provides.
  hb_run icewm cmake -G Ninja -S "$src" -B "$b" \
    -DCMAKE_TOOLCHAIN_FILE="$HB_X11_DIR/cross/toolchain-hobbyos-c.cmake" \
    -DHOBBYOS_ARCH="$HB_ARCH" -DHOBBYOS_X11_TOOLS="$HB_TOOLS" -DHOBBYOS_X11_PREFIX="$HB_PREFIX" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/ICEWM \
    -DCMAKE_CXX_FLAGS="$cxxflags" \
    -DCMAKE_EXE_LINKER_FLAGS="-L$HB_PREFIX/lib -L$HB_OBJ_DIR" \
    -DBUILD_TESTING=OFF \
    -DDATADIR=/ICEWM -DLIBDIR=/ICEWM -DCFGDIR=/ICEWM -DDOCDIR=/ICEWM/DOC -DLOCDIR=/ICEWM/LOCALE \
    -DICEHELPIDX=/ICEWM/DOC/icewm.html -DBINDIR=/ \
    -DCONFIG_DEFAULT_THEME=icedesert/default.theme \
    -DXTERMCMD=XEYESR.BIN \
    -DCONFIG_IMLIB2=OFF -DCONFIG_GDK_PIXBUF_XLIB=OFF -DCONFIG_LIBRSVG=OFF -DCONFIG_NANOSVG=OFF \
    -DCONFIG_XPM=ON -DCONFIG_LIBPNG=ON -DCONFIG_LIBJPEG=ON \
    -DCONFIG_I18N=OFF -DENABLE_NLS=OFF -DCONFIG_SESSION=OFF -DCONFIG_XRANDR=ON \
    -DCONFIG_XFREETYPE=ON -DCONFIG_COREFONTS=ON -DHAVE_XRES=OFF -DXINERAMA=ON \
    -DCONFIG_FRIBIDI=OFF -DCONFIG_FDO_MENUS=OFF -DCONFIG_SHAPE=ON -DICESOUND=none \
    -DENABLE_LTO=OFF -DCONFIG_EXTERNAL_TRAY=OFF \
    -DHAVE_BACKTRACE_SYMBOLS_FD=0 -DHAVE_MEMRCHR=1 -DHAVE_STRLCAT=0 -DHAVE_STRLCPY=0 \
    -DHAVE_SYSCTL=0 -DHAVE_SYSCTLBYNAME=0 -DHAVE_ARC4RANDOM_UNIFORM=0 \
    -DHAVE_EXECINFO_H=0 -DHAVE_SCHED_H=1 -DHAVE_SYS_SCHED_H=0 -DHAVE_SYS_SYSCTL_H=0 \
    -DHAVE_UVM_UVM_PARAM_H=0 -DHAVE_SYS_SOUNDCARD_H=0 -DHAVE_XINTERNATOMS=1 \
    -DCXX_VHI=0 -DCXX_VIHI=0 -DUSE_WLASNEEDED=0 \
    -DCMAKE_C_STANDARD_LIBRARIES="" \
    -DCMAKE_CXX_STANDARD_LIBRARIES="-lhbicecompat"
  hb_run icewm ninja -C "$b" -j"$HB_JOBS" icewm icewmbg icesh icewmhint
  local t n
  for t in icewm:ICEWM icewmbg:ICEWMBG icesh:ICESH icewmhint:ICEWMHNT; do
    n="${t##*:}"
    # The ELF (symbols) stays in obj/: bin/ is copied wholesale to the disk.
    llvm-objcopy -O binary "$b/${t%%:*}" "$HB_BIN_DIR/$n.BIN"
    hb_log "icewm: $n.BIN ($(stat -c %s "$HB_BIN_DIR/$n.BIN") bytes)"
  done
  printf '%s\n' "$key" > "$stamp"
}

# /ICEWM data tree (icewm.md §4.4): HobbyOS config files from icewm/data/
# plus upstream lib/ assets: icons, ledclock, mailbox, taskbar and the
# themes icedesert (CONFIG_DEFAULT_THEME) + win95.  Names keep their
# upstream (VFAT long, mixed-case) spelling: the kernel's FAT16 driver
# resolves long names case-insensitively (src/kernel/fat16.c lfn_*).
IW_THEMES="icedesert win95"
_iw_data() {
  hb_want icewm-data || { hb_log "icewm-data: skipped (--only)"; return 0; }
  local x="$HB_OBJ_DIR/icewm-data"; rm -rf "$x"; mkdir -p "$x"
  tar -C "$x" --strip-components=1 -xzf "$HB_DL/$IW_TB" "icewm-$IW_VER/lib"
  local out="$HB_DISK_DIR/ICEWM"
  rm -rf "$out"; mkdir -p "$out/themes"
  local d f
  for d in icons ledclock mailbox taskbar; do
    cp -R "$x/lib/$d" "$out/$d"
  done
  for d in $IW_THEMES; do
    [ -f "$x/lib/themes/$d/default.theme" ] || hb_die "icewm-data: theme $d missing upstream"
    cp -R "$x/lib/themes/$d" "$out/themes/$d"
  done
  for f in "$HB_X11_DIR"/icewm/data/*; do
    install -m 0644 "$f" "$out/$(basename "$f")"
  done
  find "$out" -name '*.in' -delete
  install -m 0644 "$x/COPYING" "$out/COPYING" 2>/dev/null \
    || tar -xzOf "$HB_DL/$IW_TB" "icewm-$IW_VER/COPYING" > "$out/COPYING"
  hb_log "icewm-data: $out ($(find "$out" -type f | wc -l) files, $(du -sh "$out" | cut -f1))"
}

# "clients" sub-step: the in-tree shim demos rebuilt against the real libX11
# (src/user/x11/apps/*/main.c compiled unchanged; xcalc gets <X11/Xutil.h>
# force-included because the shim's Xlib.h also declares XLookupString).
# The ELFs stay in obj/ (bin/ goes to the disk wholesale).
_iw_clients() {
  hb_want clients || { hb_log "clients: skipped (--only)"; return 0; }
  local o="$HB_OBJ_DIR/xclients"; mkdir -p "$o"
  : > "$HB_LOG_DIR/clients.log"
  local m app bin extra
  for m in xeyes:XEYESR xcalc:XCALCR; do
    app="${m%%:*}"; bin="${m##*:}"; extra=""
    [ "$app" = xcalc ] && extra="-include X11/Xutil.h"
    # shellcheck disable=SC2046,SC2086
    hb_run clients "$CC" $CFLAGS $("$PKG_CONFIG" --cflags x11) $extra \
      -c "$HB_REPO/src/user/x11/apps/$app/main.c" -o "$o/$app.o"
    # shellcheck disable=SC2046
    hb_run clients hb_link_flat "$o/$bin.BIN" "$o/$app.o" $("$PKG_CONFIG" --libs x11)
    cp "$o/$bin.BIN" "$HB_BIN_DIR/$bin.BIN"
    hb_log "clients: $bin.BIN ($(stat -c %s "$HB_BIN_DIR/$bin.BIN") bytes)"
  done
}

# "hbhost" sub-step (icewm.md §9, lane apps): HBHOST.BIN, the native-app
# host -- one native HobbyOS app in a real X window over a virtual display.
# Compiles the default desktop's own window-protocol code unchanged
# (src/user/desktop.c via hbhost_desk.c, src/user/graphics/{graphics,window}.c,
# src/user/games_scan.c) with this stage's CC, plus the libX11 side.
# Runs with "clients" (or alone: --only hbhost).
_iw_hbhost() {
  { hb_want hbhost || hb_want clients; } || { hb_log "hbhost: skipped (--only)"; return 0; }
  local o="$HB_OBJ_DIR/hbhost"; mkdir -p "$o"
  : > "$HB_LOG_DIR/hbhost.log"
  local inc="-I$HB_REPO/src/user_include/graphics -I$HB_REPO/src/user/hbhost"
  local f objs=""
  for f in hbhost/hbhost.c hbhost/hbhost_desk.c graphics/graphics.c graphics/window.c games_scan.c; do
    local n; n=$(basename "$f" .c)
    # shellcheck disable=SC2046,SC2086
    hb_run hbhost "$CC" $CFLAGS $inc $("$PKG_CONFIG" --cflags x11) -c "$HB_REPO/src/user/$f" -o "$o/$n.o"
    objs="$objs $o/$n.o"
  done
  # shellcheck disable=SC2046,SC2086
  hb_run hbhost hb_link_flat "$o/HBHOST.BIN" $objs $("$PKG_CONFIG" --libs x11)
  cp "$o/HBHOST.BIN" "$HB_BIN_DIR/HBHOST.BIN"
  hb_log "hbhost: HBHOST.BIN ($(stat -c %s "$HB_BIN_DIR/HBHOST.BIN") bytes)"
}

# Host golden test of icewm/compat (fnmatch/wordexp/glob vs glibc).
_iw_hosttest() {
  hb_want icewm-hosttest || return 0
  local t="$HB_OBJ_DIR/icewm-hosttest"; rm -rf "$t"; mkdir -p "$t"
  local inc="$HB_X11_DIR/icewm/include" c="$HB_X11_DIR/icewm/compat" hcc="${CC_FOR_BUILD:-cc}"
  : > "$HB_LOG_DIR/icewm-hosttest.log"
  hb_run icewm-hosttest $hcc -O1 -I"$inc" -Dfnmatch=hb_fnmatch -c "$c/hb_fnmatch.c" -o "$t/f.o"
  hb_run icewm-hosttest $hcc -O1 -I"$inc" -Dwordexp=hb_wordexp -Dwordfree=hb_wordfree -c "$c/hb_wordexp.c" -o "$t/w.o"
  hb_run icewm-hosttest $hcc -O1 -I"$inc" -Dfnmatch=hb_fnmatch -Dglob=hb_glob -Dglobfree=hb_globfree -c "$c/hb_glob.c" -o "$t/g.o"
  hb_run icewm-hosttest $hcc -O1 "$HB_X11_DIR/icewm/test/compat_host_test.c" "$t/f.o" "$t/w.o" "$t/g.o" -o "$t/compat_host_test"
  mkdir -p "$t/tree"
  tar -C "$t/tree" --strip-components=1 -xzf "$HB_DL/$IW_TB" "icewm-$IW_VER/lib"
  hb_run icewm-hosttest "$t/compat_host_test" "$t/tree/lib"
  hb_log "icewm-hosttest: $(tail -1 "$HB_LOG_DIR/icewm-hosttest.log")"
}

stage_icewm() {
  _iw_hosttest
  _iw_build
  _iw_data
  _iw_clients
  _iw_hbhost
}
