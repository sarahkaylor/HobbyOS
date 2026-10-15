# w10-fstime FAILING-FIRST probe (pre-fix kernel):
#  (1) a fresh in-OS file stats as mtime=0 / date 1980, and touch never
#      advances the timestamp;
#  (2) consequently in-OS make CANNOT do the right thing: it either sees
#      all-zero equal mtimes (spurious up-to-date) or, on files whose
#      staged source has a real host date, a target OLDER than its source
#      (spurious full rebuild).  The discriminator below: touch the
#      source, then make — a correct filesystem MUST rebuild A.O, the
#      pre-fix kernel does NOT (mtime never moved).
echo W10PRE-STEP1-CREATE-DATES
cd /W10
/RM.BIN A.TXT
/TOUCH.BIN A.TXT
/LS.BIN -l
/W10/STATP.BIN A.TXT
echo W10PRE-STEP2-MAKE-RUN1
cd /W10/P1
/RM.BIN A.O
/MAKE.BIN -f Makefile all
echo W10PRE-STEP3-TOUCH-SOURCE-THEN-MAKE
/TOUCH.BIN A.C
/W10/STATP.BIN A.C
/W10/STATP.BIN A.O
/MAKE.BIN -f Makefile all
echo W10PRE-DONE
