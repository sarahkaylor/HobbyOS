import re, subprocess

out = subprocess.run(["readelf", "-S", "-W", "hobbyos.elf"], capture_output=True, text=True).stdout
m = re.search(r"\.text\s+\w+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", out)
addr, off, size = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)
print(f".text addr=0x{addr:x} off=0x{off:x} size=0x{size:x}")

sym = subprocess.run(["nm", "hobbyos.elf"], capture_output=True, text=True).stdout
start = int([l.split()[0] for l in sym.splitlines() if l.endswith(" enter_user_space")][0], 16)
nxt = min(int(l.split()[0], 16) for l in sym.splitlines()
          if len(l.split()) == 3 and int(l.split()[0], 16) > start)
print(f"enter_user_space 0x{start:x} .. next symbol 0x{nxt:x}")

with open("hobbyos.elf", "rb") as f:
    f.seek(off + (start - addr))
    blob = f.read(nxt - start)
open("evidence/gb-100/enter_user_space.bin", "wb").write(blob)

d = subprocess.run(["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel",
                    "evidence/gb-100/enter_user_space.bin"], capture_output=True, text=True).stdout
rets = [l for l in d.splitlines() if re.search(r"\bret\b", l)]
iret = [l for l in d.splitlines() if "iret" in l]
print("bytes:", len(blob))
print("RET instructions:", len(rets))
for l in rets:
    print("  ", l.strip())
print("IRET instructions:", len(iret))
for l in iret:
    print("  ", l.strip())
open("evidence/gb-100/04-disasm-enter_user_space.txt", "w").write(d)