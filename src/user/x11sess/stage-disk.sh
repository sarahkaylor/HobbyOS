#!/bin/sh
# stage-disk.sh DISK X11_BUILD_DIR XSESSION_BIN [MCOPY] [MMD]
#
# Puts the DESKTOP=icewm payload on a freshly built HobbyOS disk image
# (icewm.md §4.3/§4.4).  Called from the Makefile disk.img recipe only when
# DESKTOP=icewm, after every DESKTOP=hobby file is already on the disk.
#
#   X11_BUILD_DIR/bin/*.BIN  -> ::/<NAME>.BIN   (8.3 uppercase names)
#   X11_BUILD_DIR/disk/<tree> -> ::/<tree>      (e.g. /X11, /ICEWM; VFAT long
#                                                names are kept — the kernel's
#                                                FAT16 driver reads and writes
#                                                them, case-insensitively)
#   XSESSION_BIN              -> ::/XSESSION.BIN (always this tree's build)
set -eu
DISK=$1
XDIR=$2
XSESS=$3
MCOPY=${4:-mcopy}
MMD=${5:-mmd}

[ -f "$XSESS" ] || { echo "[x11-stage] ERROR: $XSESS missing"; exit 1; }
[ -d "$XDIR/bin" ] || { echo "[x11-stage] ERROR: $XDIR/bin missing"; exit 1; }

n=0
for f in "$XDIR"/bin/*; do
  [ -f "$f" ] || continue
  name=$(basename "$f")
  case "$name" in
    XSESSION.BIN) continue ;; # ours wins (below)
  esac
  "$MCOPY" -o -i "$DISK" "$f" "::/$name"
  n=$((n + 1))
done
for b in XHOBBY.BIN ICEWM.BIN ICEWMBG.BIN; do
  [ -f "$XDIR/bin/$b" ] || echo "[x11-stage] WARN: $XDIR/bin/$b missing"
done
"$MCOPY" -o -i "$DISK" "$XSESS" ::/XSESSION.BIN
echo "[x11-stage] $n binaries from $XDIR/bin + XSESSION.BIN"

if [ -d "$XDIR/disk" ]; then
  # Recreate the directory tree first (mmd ignores nothing, so test each),
  # then copy the files one by one; mcopy -s would choke on dirs that
  # already exist on the image (e.g. /tmp, /home).
  (cd "$XDIR/disk" && find . -mindepth 1 -type d | sort) | while read -r d; do
    d=${d#./}
    mdir -i "$DISK" "::/$d" >/dev/null 2>&1 || "$MMD" -i "$DISK" "::/$d"
  done
  list=$(mktemp)
  (cd "$XDIR/disk" && find . -type f | sort) > "$list"
  m=0
  while IFS= read -r f; do
    f=${f#./}
    "$MCOPY" -o -i "$DISK" "$XDIR/disk/$f" "::/$f"
    m=$((m + 1))
  done < "$list"
  rm -f "$list"
  echo "[x11-stage] $m data files from $XDIR/disk"
else
  echo "[x11-stage] WARN: $XDIR/disk missing - no /X11 or /ICEWM data tree on the disk"
fi
