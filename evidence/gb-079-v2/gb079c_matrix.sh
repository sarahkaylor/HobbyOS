#!/usr/bin/env bash
# GB-079-V (re-open) six-case matrix at 9e107c3, each case in a clean tree.
set +e
D=/tmp/gb079c
EV=/tmp/gb079c-ev
mkdir -p "$EV"
cd "$D" || exit 9
export PATH="/usr/sbin:/sbin:$PATH"

fresh() {  # clean tree, derived artefacts removed
  git checkout -- src/user/t4_probe.c 2>/dev/null
  rm -f obj/t4_legA.out
}

synth() {  # the capture a healthy run emits
  mkdir -p obj
  grep -v '^#' tests/fixtures/abi/t4_rows.golden | grep -v '^$' \
    | sed 's/^/row=/; s/$/ rc=0 errno=0/' > obj/t4_legA.out
}

inject() {
  python3 - <<'PY'
p = "src/user/t4_probe.c"
s = open(p).read()
a = '    prow("write3", r, (r < 0) ? (r == -1 ? 9 : (long)(-r)) : 0, msg, 3);\n'
assert s.count(a) == 1
s = s.replace(a, a + '    prow("bogus_unconditional", 7, 0, NULL, 0); /* GB-079-V injected row */\n')
open(p, "w").write(s)
PY
  echo "  injected: $(grep -c bogus_unconditional src/user/t4_probe.c) line(s) in src/user/t4_probe.c"
}

echo "== golden rows: $(grep -vc '^#' tests/fixtures/abi/t4_rows.golden)"
echo "== dent_have_charlie in golden: $(grep -c 'dent_have_charlie' tests/fixtures/abi/t4_rows.golden)"

echo; echo "########## CASE 0: --self-test at head"
fresh
bash tools/gate/t4_device.sh --self-test > "$EV/00-selftest.log" 2>&1; echo "self-test rc=$?"
tail -2 "$EV/00-selftest.log"

echo; echo "########## CASE 1: healthy / leg A absent  (want 0)"
fresh
bash tools/gate/t4_device.sh --check > "$EV/01-healthy-absent.log" 2>&1; echo "rc=$?"
grep -E 'mode=|PASS|FAIL' "$EV/01-healthy-absent.log" | tail -4

echo; echo "########## CASE 2: healthy / leg A present  (want 0)"
fresh; synth
echo "  capture rows: $(wc -l < obj/t4_legA.out)"
bash tools/gate/t4_device.sh --check > "$EV/02-healthy-present.log" 2>&1; echo "rc=$?"
grep -E 'mode=|PASS|FAIL' "$EV/02-healthy-present.log" | tail -4

echo; echo "########## CASE 3: INJECTED row / leg A present  (want NON-ZERO)  <-- THE GATE"
inject
bash tools/gate/t4_device.sh --check > "$EV/03-injected-present.log" 2>&1; echo "rc=$?"
grep -E 'mode=|PASS|FAIL|bogus' "$EV/03-injected-present.log" | tail -6

echo; echo "########## CASE 4: same injection / leg A absent  (want NON-ZERO)"
rm -f obj/t4_legA.out
bash tools/gate/t4_device.sh --check > "$EV/04-injected-absent.log" 2>&1; echo "rc=$?"
grep -E 'mode=|PASS|FAIL|bogus' "$EV/04-injected-absent.log" | tail -6

echo; echo "########## CASE 5: probe restored / stale capture missing one row  (want NON-ZERO, STALE)"
git checkout -- src/user/t4_probe.c; echo "  probe restored: $(git status --porcelain src/user/t4_probe.c | wc -l) modified file(s)"
synth
sed -i '/^row=dent_have_charlie/d' obj/t4_legA.out
echo "  stale capture rows: $(wc -l < obj/t4_legA.out)"
bash tools/gate/t4_device.sh --check > "$EV/05-stale.log" 2>&1; echo "rc=$?"
grep -E 'mode=|PASS|FAIL|STALE' "$EV/05-stale.log" | tail -6

echo; echo "########## CASE 6: everything restored  (want 0, fail_count=0)"
fresh
bash tools/gate/t4_device.sh --self-test > "$EV/06-selftest-final.log" 2>&1; echo "self-test rc=$?"
tail -2 "$EV/06-selftest-final.log"
echo; echo "== tree state: $(git status --porcelain | wc -l) modified/untracked file(s)"
