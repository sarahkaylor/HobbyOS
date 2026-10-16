# dot-dispatch + bare-name smoke, run via SH.BIN script mode on the disk.
echo "[DOT] smoke start"
cd /
./build_kernel.sh check
rc1="$?"
echo "[DOT] root ./build_kernel.sh check rc=$rc1"
if [ "x$rc1" != "x0" ]; then
  cd /SRC
  ./build_kernel.sh check
  rc1="$?"
  echo "[DOT] /SRC ./build_kernel.sh check rc=$rc1"
fi
SH.BIN -c "echo [DOT] SH.BIN bare-name resolved"
rc2="$?"
echo "[DOT] SH.BIN -c rc=$rc2"
if [ "x$rc1" = "x0" ]; then
  echo "[DOT] PASS"
  exit "$rc2"
fi
echo "[DOT] FAIL rc1=$rc1"
exit "$rc1"
