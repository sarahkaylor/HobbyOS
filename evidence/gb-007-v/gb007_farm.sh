#!/usr/bin/env bash
# GB-007-V: build a PATH that holds every ordinary tool but NOT musl-gcc,
# so t4_parity.sh takes its *true skip* path (command -v musl-gcc fails)
# while leg B still has clang + binutils to build with.
set -e
FARM=/tmp/gb007-path
mkdir -p "$FARM"
n=0; skipped=0
for f in /usr/bin/* /bin/*; do
  b=$(basename "$f")
  case "$b" in
    musl-*|musl) skipped=$((skipped+1)); continue ;;
  esac
  [ -e "$FARM/$b" ] && continue
  ln -s "$f" "$FARM/$b" 2>/dev/null && n=$((n+1))
done
echo "farm=$FARM links=$n musl_skipped=$skipped"
echo "musl-gcc visible in farm: $(PATH=$FARM command -v musl-gcc || echo NO)"
echo "clang visible in farm:    $(PATH=$FARM command -v clang || echo NO)"
echo "ld visible in farm:       $(PATH=$FARM command -v ld || echo NO)"
