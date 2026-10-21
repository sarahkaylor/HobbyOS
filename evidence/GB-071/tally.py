import re, sys
p = sys.argv[1] if len(sys.argv) > 1 else "obj/t3_intel.log"
lines = [l.rstrip("\r") for l in open(p, errors="ignore") if "memory fault VA=" in l]
print("fault lines:", len(lines))
fam = [l for l in lines if " rw=r" in l]
print("rw=r (family B shape):", len(fam), " rw=w:", len(lines) - len(fam))
rdi_eq_va = 0
syscalls = {}
pcs = {}
for l in fam:
    m = re.search(r"VA=0x([0-9A-Fa-f]+).*?rdi=0x([0-9A-Fa-f]+).*?rsi=0x([0-9A-Fa-f]+).*?PC=0x([0-9A-Fa-f]+).*?sys=(-?\d+)", l)
    if not m:
        continue
    va, rdi, rsi, pc, sc = m.groups()
    if int(va, 16) == int(rdi, 16):
        rdi_eq_va += 1
    syscalls[sc] = syscalls.get(sc, 0) + 1
    pcs[pc[-4:]] = pcs.get(pc[-4:], 0) + 1
print("family-B lines where rdi == VA (the pointer is the argument):", f"{rdi_eq_va}/{len(fam)}")
print("sys= histogram (last syscall before the fault):", syscalls)
print("PC low-16 histogram:", sorted(pcs.items(), key=lambda kv: -kv[1])[:8])
