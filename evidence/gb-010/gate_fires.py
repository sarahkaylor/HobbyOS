"""GB-010 gate-fires proof: mutate one measured number in the doc, run
`make abi_doc_check` (expect non-zero), restore the doc, re-run (expect 0)."""
import re, subprocess, sys

DOC = "docs/linux-abi.md"
orig = open(DOC).read()
m = re.search(r"^  read\s+0$", orig, re.M)
if not m:
    sys.exit("could not find the 'read 0' row to mutate")
mutated = orig[:m.start()] + "  read                   9" + orig[m.end():]
open(DOC, "w").write(mutated)
print("injected: 'read 0' -> 'read 9' in", DOC)

r = subprocess.run(["make", "abi_doc_check"], capture_output=True, text=True)
out = [l for l in (r.stdout + r.stderr).splitlines() if "STALE" in l or "matches a fresh" in l]
print("$ make abi_doc_check   # with one number mutated")
for l in out:
    print(" ", l)
print("EXIT=" + str(r.returncode))

open(DOC, "w").write(orig)
r2 = subprocess.run(["make", "abi_doc_check"], capture_output=True, text=True)
out2 = [l for l in (r2.stdout + r2.stderr).splitlines() if "STALE" in l or "matches a fresh" in l]
print("$ make abi_doc_check   # restored")
for l in out2:
    print(" ", l)
print("EXIT=" + str(r2.returncode))
