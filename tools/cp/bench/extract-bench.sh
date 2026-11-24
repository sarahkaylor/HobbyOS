#!/bin/bash
# wp-cp: pull Cyberpunk 2077's own benchmark output out of a HobbyOS ext2
# prefix image (cpdata.img) and print the numbers.
#   extract-bench.sh <cpdata.img> <outdir>
set -u
IMG=$1; OUT=${2:-/tmp/br}
rm -rf "$OUT"; mkdir -p "$OUT"
BR='/pfx/drive_c/users/root/Documents/CD Projekt Red/Cyberpunk 2077/benchmarkResults'
echo "== benchmarkResults on $IMG"
debugfs -R "ls -l \"$BR\"" "$IMG" 2>/dev/null | head -20
debugfs -R "rdump \"$BR\" \"$OUT\"" "$IMG" 2>/dev/null
find "$OUT" -type f | head -20
echo "== summary.json / frames.csv"
find "$OUT" -name summary.json -o -name frames.csv | while read -r f; do
  echo "--- $f ($(stat -c %s "$f") bytes)"
  case $f in
    *summary.json) cat "$f"; echo ;;
    *frames.csv) head -3 "$f"; echo "   ... $(wc -l < "$f") lines" ;;
  esac
done
