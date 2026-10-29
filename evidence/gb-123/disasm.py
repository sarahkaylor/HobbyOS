import re, subprocess

out = subprocess.run(["readelf", "-S", "-W", "hobbyos.elf"], capture_output=True, text=True).stdout
m = re.search(r"\.text\s+\w+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", out)
addr, off, _ = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)

syms = {}
for line in subprocess.run(["nm", "hobbyos.elf"], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)

start = syms["enter_user_space"]
nxt = min(v for v in syms.values() if v > start)
with open("hobbyos.elf", "rb") as f:
    f.seek(off + (start - addr))
    blob = f.read(nxt - start)
open("evidence/gb-123/enter_user_space.bin", "wb").write(blob)

d = subprocess.run(["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel",
                    "evidence/gb-123/enter_user_space.bin"], capture_output=True, text=True).stdout
lines = d.splitlines()
print(f"enter_user_space 0x{start:x} .. 0x{nxt:x} ({nxt-start} bytes)\n")
for l in lines[:16]:
    print("   ", l.strip())
print()
print("calls / pushes / mov rsp:")
for l in lines:
    if re.search(r"\bcall\b|\bpush\b|\bmov\s+rsp|\blea\s+rsp", l):
        print("   ", l.strip())

# frame layout offsets (x64 struct trap_frame)
print("\nframe slots: vector=tf+264 error_code=tf+272 cs=tf+280 ss=tf+288 "
      "(regs[30]=0..232, lr=240, elr=248, spsr=256)")