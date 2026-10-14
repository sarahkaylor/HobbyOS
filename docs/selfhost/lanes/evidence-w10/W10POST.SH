# w10-fstime ACCEPTANCE probe (post-fix kernel): real RTC dirent timestamps
# make in-OS make INCREMENTAL.
#   P1 = 2-TU explicit-rule project; P2 = 5-TU explicit-rule project.
#   (No pattern rules: FAT16 short names are uppercase, patterns won't match.)
#   Source rewrites use CP.BIN (O_TRUNC + write -> the kernel's real mtime
#   stamping path); the in-OS TOUCH tool only creates (does not utimens).
echo W10POST-STEP1-CREATE-DATES
cd /W10
/RM.BIN A.TXT
/TOUCH.BIN A.TXT
/LS.BIN -l
/W10/STATP.BIN A.TXT
echo W10POST-STEP2-MAKE-RUN1
cd /W10/P1
/RM.BIN A.O
/MAKE.BIN -f Makefile all
echo W10POST-STEP3-SLEEP3-THEN-MAKE2
/W10/STATP.BIN sleep 3
/MAKE.BIN -f Makefile all
echo W10POST-STEP4-REWRITE-SOURCE-THEN-MAKE
/CP.BIN A2.C A.C
/W10/STATP.BIN A.C
/MAKE.BIN -f Makefile all
echo W10POST-STEP5-5TU-FULL-BUILD
cd /W10/P2
/RM.BIN B.O C.O D.O E.O F.O
/MAKE.BIN -f Makefile all
echo W10POST-STEP6-5TU-MAKE2-AFTER-SLEEP
/W10/STATP.BIN sleep 3
/MAKE.BIN -f Makefile all
echo W10POST-STEP7-5TU-REWRITE-ONE-SOURCE
/CP.BIN D2.C D.C
/MAKE.BIN -f Makefile all
echo W10POST-DONE
