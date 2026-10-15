# w10-fstime: literal 'Nothing to be done' (recipe-less phony aggregator)
# + mkdir timestamp readback via stat.
echo W10P3-STEP1-MKDIR-STAT
/MKDIR.BIN /W10/D1
/W10/STATP.BIN /W10/D1
echo W10P3-STEP2-MAKE-FIRST
cd /W10/P3
/RM.BIN A.O
/MAKE.BIN -f Makefile all
echo W10P3-STEP3-MAKE-AGAIN-UP-TO-DATE
/MAKE.BIN -f Makefile all
echo W10P3-STEP4-REWRITE-SOURCE
/CP.BIN A2.C A.C
/MAKE.BIN -f Makefile all
echo W10P3-DONE
