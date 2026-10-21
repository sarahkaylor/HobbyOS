"""GB-071 census: parse intel T3 fault lines, classify families, and test the
delivery hypothesis "a syscall returned a kernel-range pointer".

Family B (per the ticket): rw=r, region=NONE, VA inside the running CPU's kernel
scratch frame [target_sp-296, target_sp) with target_sp = &__stack_top - cpu*0x10000 - 4096.
We cannot know cpu from the line, but any VA >= 0x70000000 is a kernel address.
"""
import re
import sys

LOG = sys.argv[1] if len(sys.argv) > 1 else "obj/t3_intel.log"
raw = open(LOG, errors="ignore").read()

KERNEL_LO = 0x70000000

# full line (unshredded)
full = re.compile(
    r"\[KERNEL\] pid=(\d+) \(([^)]*)\) memory fault VA=0x([0-9A-Fa-f]+) PC=0x([0-9A-Fa-f]+)"
    r" rw=(\w)\s+ in=(\w+)(.*?)(?:-> killed)?$", re.M)

rows = []
for m in full.finditer(raw):
    pid, name, va, pc, rw, why, rest = m.groups()
    def grab(k):
        mm = re.search(k + r"=0x([0-9A-Fa-f]+)", rest) or re.search(k + r"=(\d+)", rest)
        return int(mm.group(1), 16) if mm and mm.group(1)[0].isdigit() and len(mm.group(1)) > 6 else (int(mm.group(1)) if mm else None)
    rows.append({
        "pid": int(pid), "name": name, "va": int(va, 16), "pc": int(pc, 16),
        "rw": rw, "in": why, "rest": rest.strip(),
        "sys": grab("sys"), "ret": (int(re.search(r"ret=0x([0-9A-Fa-f]+)", rest).group(1), 16)
                                   if re.search(r"ret=0x([0-9A-Fa-f]+)", rest) else None),
    })

total_raw = len(re.findall(r"memory fault VA=", raw))
print(f"raw 'memory fault VA=' occurrences: {total_raw}")
print(f"parsed lines with pid/PC/rw:        {len(rows)}")
print(f"parse coverage:                     {100.0*len(rows)/max(1,total_raw):.1f}%")
print()

famB = [r for r in rows if r["rw"] == "r" and r["va"] >= KERNEL_LO]
famA = [r for r in rows if r["rw"] == "w" and "region=NONE" in r["rest"]]
print(f"family-B shape (rw=r, VA>=0x{KERNEL_LO:x}, kernel range): {len(famB)}")
print(f"family-A-ish (rw=w, region=NONE):                        {len(famA)}")
print()

print("=== family B rows (pid, name, VA, PC, sys, ret) ===")
for r in famB:
    print(f"  pid={r['pid']:<3} {r['name']:<14} VA=0x{r['va']:012x} PC=0x{r['pc']:x} "
          f"sys={r['sys']} ret=0x{(r['ret'] or 0):012x}  in={r['in']}")
print()

def census(key):
    d = {}
    for r in famB:
        d[r[key]] = d.get(r[key], 0) + 1
    return sorted(d.items(), key=lambda kv: -kv[1])

print("family B by syscall number:", census("sys"))
print("family B by process:       ", census("name"))
kernel_ret = [r for r in famB if r["ret"] and r["ret"] >= KERNEL_LO]
print(f"\nfamily-B faults whose last syscall RETURN (ret) is a kernel address: {len(kernel_ret)}")
for r in kernel_ret[:10]:
    print(f"  pid={r['pid']} {r['name']} sys={r['sys']} ret=0x{r['ret']:x} VA=0x{r['va']:x}")
