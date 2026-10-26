#!/usr/bin/env bash
# make a CLEAN T3-style log (>1000 benign lines, System halt, 55 programs, no FAIL,
# no kernel fault) + an injected-FAIL variant, run t3_tier.sh police on both.
# -> clean EXIT 0 (PASS), injected FAIL EXIT 1 (FAIL): grading untouched (GB-103).
f=/tmp/t3_clean.log
: > "$f"
i=0
for n in $(seq 1 60); do echo "Process $n: APP$n.BIN exited"; done >> "$f"
while [ "$(wc -l < "$f")" -lt 1150 ]; do echo "some benign console chatter line $((i++))"; done >> "$f"
printf 'System halt\n' >> "$f"
cp "$f" /tmp/t3_fail.log
printf 'SOMETEST: FAIL injected failure\n' >> /tmp/t3_fail.log
echo "== police on CLEAN log =="
bash tools/t3_tier.sh police "$f" 2>&1 | tail -3
echo "clean-exit=${PIPESTATUS[0]}"
echo "== police on INJECTED-FAIL log =="
bash tools/t3_tier.sh police /tmp/t3_fail.log 2>&1 | tail -3
echo "fail-exit=${PIPESTATUS[0]}"
