# BENCH.SH — w11-makej in-OS synthetic-build benchmark driver (v2).
#
# Written for the in-OS sh dialect AFTER empirical probing (probe1.serial):
#   - empty double-quoted args are DROPPED by the tokenizer (they shift
#     positional params), so every function argument below is non-empty;
#   - `while` + `n=$((n+1))` does not advance the variable (infinite loop),
#     so the 16-output comparison is UNROLLED with literal names;
#   - sh functions + non-empty positionals work (probe P2).
#
# Run by the SELFHOST driver as:  sh /SRC/BM/BENCH.SH <mode>
# The driver spawns every stage tool with cwd=/, so we cd first.  Each mode
# runs ONE make invocation plus correctness checks; the SELFHOST driver
# wraps every stage with [SELFHOST] phase=<name>-start/-done markers that
# the host runner (tools/w11_bench_boot.py) timestamps -> stage wall time.
#
# Modes: ser1 ser2 (-j1, O1), j41 j42 (-j4, O4), j81 j82 (-j8, O8),
#        failser (-j1 MKFAIL, OF), failj8 (-j8 MKFAIL, OF2).
# -B forces a rebuild every run so repeated timings ignore FAT timestamps.
cd /SRC/BM
MODE=$1

cmp_all() {   # $1 = outdir (non-empty).  16 literal compares vs sources.
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU01.C $1/TU01.O; echo "BM-CMP TU01 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU02.C $1/TU02.O; echo "BM-CMP TU02 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU03.C $1/TU03.O; echo "BM-CMP TU03 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU04.C $1/TU04.O; echo "BM-CMP TU04 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU05.C $1/TU05.O; echo "BM-CMP TU05 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU06.C $1/TU06.O; echo "BM-CMP TU06 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU07.C $1/TU07.O; echo "BM-CMP TU07 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU08.C $1/TU08.O; echo "BM-CMP TU08 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU09.C $1/TU09.O; echo "BM-CMP TU09 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU10.C $1/TU10.O; echo "BM-CMP TU10 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU11.C $1/TU11.O; echo "BM-CMP TU11 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU12.C $1/TU12.O; echo "BM-CMP TU12 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU13.C $1/TU13.O; echo "BM-CMP TU13 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU14.C $1/TU14.O; echo "BM-CMP TU14 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU15.C $1/TU15.O; echo "BM-CMP TU15 = $?"
  /TOOLS/CMP.BIN -s /SRC/BM/SRC/TU16.C $1/TU16.O; echo "BM-CMP TU16 = $?"
  /TOOLS/PS.BIN
}

bench() {   # $1 = mode  $2 = make flags (never empty)  $3 = outdir  $4 = makefile
  /TOOLS/MAKE.BIN $2 -B -f /SRC/BM/$4 OUTDIR=$3 all
  mc=$?
  echo "BM-EXIT $1 $3 = $mc"
  cmp_all $3
  if [ $mc -ne 0 ]; then
    case $1 in
      failser) echo "BM-EXPECTED-FAIL ser make_rc=$mc" ;;
      failj8) echo "BM-EXPECTED-FAIL j8 make_rc=$mc" ;;
      *) echo "BM-UNEXPECTED-FAIL $1 rc=$mc"
         return 1 ;;
    esac
  fi
  return 0
}

case $MODE in
  ser1)   bench ser1   -j1 /SRC/BM/O1 MK16.MK ;;
  ser2)   bench ser2   -j1 /SRC/BM/O1 MK16.MK ;;
  j41)    bench j41    -j4 /SRC/BM/O4 MK16.MK ;;
  j42)    bench j42    -j4 /SRC/BM/O4 MK16.MK ;;
  j81)    bench j81    -j8 /SRC/BM/O8 MK16.MK ;;
  j82)    bench j82    -j8 /SRC/BM/O8 MK16.MK ;;
  failser) bench failser -j1 /SRC/BM/OF MKFAIL.MK ;;
  failj8)  bench failj8  -j8 /SRC/BM/OF2 MKFAIL.MK ;;
  *) echo "BM-BADMODE $MODE"; exit 1 ;;
esac
