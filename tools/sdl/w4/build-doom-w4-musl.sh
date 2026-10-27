# Doom W4 static-musl ELF — build profile (GB-041)
#
# Reproducible recipe that cross-compiles the doomgeneric engine (ozkl/doomgeneric,
# GPLv2+; already vendored at third_party/doomgeneric) to a **static-musl x86_64
# ELF** for the HobbyOS W4 compat load path. This is the W4 port surface: the
# engine object set compiles under `clang --target=x86_64-linux-musl` and links
# statically with a minimal DG_* backend, producing a non-PIE static ELF.
#
# This is the GB-041 port surface. Runtime *playability* in-guest is the compat
# gate and depends on GB-033/GB-031 (the /dev/fb0 fbcon video + loader-acceptance
# halves), tracked there. This script produces the build-level artifact.
#
# Usage:   tools/sdl/w4/build-doom-w4-musl.sh [dest_dir]
# Output:  <dest>/DoomW4.elf   (ELF 64-bit LSB executable, x86-64, statically linked)
set -uo pipefail
DEST="${1:-$(pwd)/doom-w4-build}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
mkdir -p "$DEST"
cd "$ROOT"

ENG="$ROOT/third_party/doomgeneric"
CFLAGS="--target=x86_64-linux-musl -O2 -fPIE -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 -D_DEFAULT_SOURCE -include unistd.h -I $ENG"

# Engine module set, trimmed to the real .c files in SRC_DOOM (never a wildcard:
# upstream pulls in _sdl/_xlib platform backends).
SRC=(dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric)

for n in "${SRC[@]}"; do
  clang $CFLAGS -c "$ENG/$n.c" -o "$DEST/$n.o"
done
clang $CFLAGS -c "$ROOT/tools/sdl/w4/doomgeneric_w4backend.c" -o "$DEST/doomgeneric_w4backend.o"
clang --target=x86_64-linux-musl -static-pie -fPIE "$DEST"/*.o -lm -o "$DEST/DoomW4.elf"

echo
echo "== DoomW4.elf =="
file "$DEST/DoomW4.elf"
sha256sum "$DEST/DoomW4.elf" | tee "$DEST/DoomW4.elf.sha256"
