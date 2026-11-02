# Doom W4 static-musl ELF — build profile (GB-041)
#
# Reproducible recipe that cross-compiles the doomgeneric engine (ozkl/doomgeneric,
# GPLv2+; already vendored at third_party/doomgeneric) to a **static-musl x86_64
# ELF** for the HobbyOS W4 compat load path. This is the W4 port surface: the
# engine object set compiles under `clang --target=x86_64-linux-musl` and links
# statically with a minimal DG_* backend, producing a static-PIE ELF (Type DYN).
#
# This is the GB-041 port surface. Runtime *playability* in-guest is the compat
# gate and depends on GB-033/GB-031 (the /dev/fb0 fbcon video + loader-acceptance
# halves), tracked there. This script produces the build-level artifact.
#
# Usage:   tools/sdl/w4/build-doom-w4-musl.sh [dest_dir]
# Output:  <dest>/DoomW4.elf   (ELF 64-bit LSB pie executable, x86-64, static-pie linked)
set -uo pipefail
DEST="${1:-$(pwd)/doom-w4-build}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
mkdir -p "$DEST"
cd "$ROOT"

ENG="$ROOT/third_party/doomgeneric"
# GB-170: `clang --target=x86_64-linux-musl` has no musl sysroot on this box, so
# it fell through to the host's glibc headers/libs and the "musl" artifact was a
# glibc static-PIE (strings: glibc-ldI3/glibc-ldH3; nanosleep -> clock_nanosleep(230),
# which the compat table does not map).  Use the musl driver that IS on the box
# and fail closed if the link still ends up glibc.
CC="${CC:-musl-gcc}"
CFLAGS="-O2 -fPIE -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 -D_DEFAULT_SOURCE -include unistd.h -I $ENG"

# Stale objects from an earlier (glibc) build must not be linked into the musl
# artifact.
rm -f "$DEST"/*.o

# Engine module set, trimmed to the real .c files in SRC_DOOM (never a wildcard:
# upstream pulls in _sdl/_xlib platform backends).
SRC=(dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric)

for n in "${SRC[@]}"; do
  $CC $CFLAGS -c "$ENG/$n.c" -o "$DEST/$n.o" || exit 1
done
$CC $CFLAGS -c "$ROOT/tools/sdl/w4/doomgeneric_w4backend.c" -o "$DEST/doomgeneric_w4backend.o" || exit 1
$CC -static-pie -fPIE "$DEST"/*.o -lm -o "$DEST/DoomW4.elf" || exit 1

# Fail closed (GB-170): the old recipe's musl target silently degraded to glibc.
if strings "$DEST/DoomW4.elf" | grep -q "glibc-"; then
  echo "build-doom-w4-musl: ERROR: DoomW4.elf links glibc, not musl (CC=$CC)" >&2
  exit 1
fi
if ! objdump -d --disassemble=nanosleep "$DEST/DoomW4.elf" 2>/dev/null | grep -q "syscall"; then
  echo "build-doom-w4-musl: ERROR: musl nanosleep does not reach a direct syscall (glibc clock_nanosleep fallback?)" >&2
  exit 1
fi

echo
echo "== DoomW4.elf =="
file "$DEST/DoomW4.elf"
sha256sum "$DEST/DoomW4.elf" | tee "$DEST/DoomW4.elf.sha256"
