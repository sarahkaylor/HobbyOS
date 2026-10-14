# PROBE.SH — w11-makej shell-dialect probe (diagnostic only).
# Exercises the exact constructs the benchmark script uses, to pin down
# which ones the in-OS sh mis-parses before I rewrite.
echo "P1 START"
bench() {
  echo "P2 func 1=$1 2=$2 3=$3 4=$4"
}
bench AA "" BB CC
bench BB -j8 /SRC/BM/O1 MK16.MK
echo "P3 lastrc=$?"
n=1
while [ $n -le 3 ]; do
  echo "P4-W n=$n"
  n=$((n+1))
done
echo "P5 while-done n=$n"
f=x
f=0$n
echo "P6 cat-assign f=$f"
if [ 3 -lt 10 ]; then echo "P7 lt-ok"; else echo "P7 lt-FAIL"; fi
case XZ in
  ser1|ser2) echo "P8 case-nodefault" ;;
  *) echo "P8 case-other" ;;
esac
/TOOLS/MAKE.BIN -B -f /SRC/BM/MK16.MK OUTDIR=/SRC/BM/O1 all
echo "P9 make-rc=$?"
