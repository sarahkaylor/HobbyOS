"""GB-010 DoD proof for the merge gate: one number mutated -> DOCS_abi must FAIL
and the gate must exit non-zero; restored -> PASS. Leaves the doc as it was."""
import re, subprocess, sys

DOC = "docs/linux-abi.md"
orig = open(DOC).read()
m = re.search(r"^  read\s+0$", orig, re.M)
if not m:
    sys.exit("row to mutate not found")
open(DOC, "w").write(orig[:m.start()] + "  read                   9" + orig[m.end():])
print("injected: 'read 0' -> 'read 9'")

for label in ("mutated", "restored"):
    r = subprocess.run(["bash", "tools/gate/merge_gate.sh"], capture_output=True, text=True,
                       env={"PATH": "/usr/local/bin:/usr/bin:/bin", "GATE_T1": "0", "HOME": "/home/sarah"})
    lines = [l for l in (r.stdout + r.stderr).splitlines() if "DOCS_abi" in l or "GATE:" in l]
    print(f"$ GATE_T1=0 bash tools/gate/merge_gate.sh   # {label}")
    for l in lines:
        print("  " + l)
    print("  EXIT=" + str(r.returncode))
    if label == "mutated":
        open(DOC, "w").write(orig)
