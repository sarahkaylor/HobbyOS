# shellcheck shell=bash
# build-clientlibs.sh -- stage `clientlibs` (lane xlibs-core), sourced by build.sh.
# Static .a only, both arches: zlib, libpng, freetype, xorgproto, xtrans,
# libXau, xcb-proto (host python codegen data), libxcb, libX11, libXext,
# libXrender, libXfixes.

_cl_zlib() {
  local pkg=zlib tb=zlib-1.3.1.tar.gz
  hb_want $pkg || return 0
  local key; key="$(hb_key $pkg $tb r1)"
  if hb_done $pkg "$key"; then hb_log "$pkg: cached"; return 0; fi
  (
    hb_pkg_lock $pkg
    hb_done $pkg "$key" && exit 0
    hb_log "$pkg: building"; : > "$HB_LOG_DIR/$pkg.log"
    local src; src="$(hb_src $pkg $tb)"
    # zlib's hand-written configure: in-tree, CHOST selects cross tools.
    cd "$src"
    hb_run $pkg env CHOST="$HB_TRIPLE" CFLAGS="$CFLAGS" ./configure --static --prefix="$HB_PREFIX"
    # Archive only: the example/minigzip programs are not wanted.
    hb_run $pkg make -j"$HB_JOBS" libz.a
    hb_run $pkg make install
    hb_mark $pkg "$key"; hb_log "$pkg: ok"
  )
}

_cl_png() {
  local neon=""
  # NEON/SSE intrinsics paths off: plain C filters (no runtime CPU probing).
  case "$HB_ARCH" in arm) neon="--enable-arm-neon=no";; intel) neon="--enable-intel-sse=no";; esac
  hb_autotools libpng libpng-1.6.44.tar.xz \
    --make "libpng16.la" \
    --install "install-libLTLIBRARIES install-pkgincludeHEADERS install-nodist_pkgincludeHEADERS install-pkgconfigDATA install-data-hook install-header-links install-library-links install-libpng-pc" \
    -- $neon --without-binconfigs --disable-tools --disable-tests
}

_cl_freetype() {
  hb_autotools freetype freetype-2.14.3.tar.xz -- \
    --with-zlib=yes --with-png=yes --with-bzip2=no --with-brotli=no --with-harfbuzz=no \
    --enable-freetype-config
}

_cl_xorgproto() { hb_autotools xorgproto xorgproto-2026.1.tar.xz -- --disable-specs --without-xmlto --without-fop --without-xsltproc; }
_cl_xtrans()    { hb_autotools xtrans xtrans-1.6.0.tar.xz -- --disable-docs --without-xmlto --without-fop --without-xsltproc; }
_cl_xau()       { hb_autotools libXau libXau-1.0.12.tar.xz -- --disable-docs --without-xmlto --without-fop --without-xsltproc; }
_cl_xcbproto()  { PYTHON=python3 hb_autotools xcb-proto xcb-proto-1.17.0.tar.xz -- PYTHON=python3; }

_cl_pthstubs() { hb_autotools libpthread-stubs libpthread-stubs-0.5.tar.xz; }

_cl_xcb() {
  # Extensions: keep the ones a WM/X server/clients stack plausibly needs;
  # the rest are compiled anyway by default and cost only archive size.
  hb_autotools libxcb libxcb-1.17.0.tar.xz -- PYTHON=python3 \
    --disable-devel-docs --without-doxygen --without-launchd \
    --disable-selinux --disable-xevie --disable-xprint --disable-xinput \
    --disable-xkb --disable-xtest --disable-dri2 --disable-dri3 --disable-present \
    --disable-sync --enable-xfixes --enable-render --enable-shape --enable-randr \
    --enable-xinerama --enable-composite --enable-damage --disable-shm \
    --disable-xv --disable-xvmc --disable-dpms --disable-screensaver --disable-record \
    --disable-res --disable-glx
}

_cl_x11() {
  # Static, no loadable i18n/XKB modules, no XF86BigFont, no xcms DB.
  # malloc0returnsnull: HobbyOS malloc(0) returns a unique pointer (glibc
  # semantics) -> "no" (cross-compiling cannot run the probe).
  hb_autotools libX11 libX11-1.8.14.tar.xz -- \
    --disable-specs --without-xmlto --without-fop --without-xsltproc --without-perl \
    --disable-loadable-i18n --disable-loadable-xcursor --disable-xf86bigfont \
    --disable-xthreads --disable-ipv6 --disable-tcp-transport --enable-unix-transport \
    --disable-local-transport --enable-malloc0returnsnull=no --disable-xlocaledir
}

_cl_xext()    { hb_autotools libXext libXext-1.3.7.tar.xz -- --disable-specs --without-xmlto --without-fop --without-xsltproc --enable-malloc0returnsnull=no; }
_cl_xrender() { hb_autotools libXrender libXrender-0.9.13.tar.xz -- --enable-malloc0returnsnull=no; }
_cl_xfixes()  { hb_autotools libXfixes libXfixes-6.0.2.tar.xz; }

stage_clientlibs() {
  _cl_zlib
  _cl_png
  _cl_freetype
  _cl_xorgproto
  _cl_xtrans
  _cl_xau
  _cl_xcbproto
  _cl_pthstubs
  _cl_xcb
  _cl_x11
  _cl_xext
  _cl_xrender
  _cl_xfixes
}
