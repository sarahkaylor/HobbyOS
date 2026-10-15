# PROBE2.SH — w11-makej smoke: v2-construct serial + -j8 build on the
# synthetic project (no while/empty-arg/arithmetic constructs).
echo "Q1 START"
/TOOLS/MAKE.BIN -B -j1 -f /SRC/BM/MK16.MK OUTDIR=/SRC/BM/O1 all
echo "Q2 serial rc=$?"
/TOOLS/CMP.BIN -s /SRC/BM/SRC/TU01.C /SRC/BM/O1/TU01.O
echo "Q3 cmp01 rc=$?"
/TOOLS/MAKE.BIN -B -j8 -f /SRC/BM/MK16.MK OUTDIR=/SRC/BM/O8 all
echo "Q4 j8 rc=$?"
/TOOLS/CMP.BIN -s /SRC/BM/SRC/TU01.C /SRC/BM/O8/TU01.O
echo "Q5 cmp01j8 rc=$?"
/TOOLS/PS.BIN
echo "Q6 DONE"
