import re, subprocess, sys

# Where do the frame_check's two saves and its call return address land, relative
# to the frame base (frame_offset 0 = the first hardware word, RIP)?
#
# usage: disasm_exit.py <elf> <label>

elf, label = sys.argv[1], sys.argv[2]

out = subprocess.run(["readelf", "-S", "-W", elf], capture_output=True, text=True).stdout
m = re.search(r"\.text\s+\w+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", out)
addr, off = int(m.group(1), 16), int(m.group(2), 16)

syms = {}
for line in subprocess.run(["nm", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)

start = syms["common_trap_exit"]
nxt = min(v for v in syms.values() if v > start)
with open(elf, "rb") as f:
    f.seek(off + (start - addr))
    blob = f.read(nxt - start)
open("/tmp/_gb101v4_tail.bin", "wb").write(blob)

d = subprocess.run(["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel",
                    "/tmp/_gb101v4_tail.bin"], capture_output=True, text=True).stdout

print(f"== {label}: common_trap_exit 0x{start:x}..0x{nxt:x} ({nxt-start} bytes)")
rsp = 0
pending_lea = None
for raw in d.splitlines():
    s = raw.strip()
    mm = re.search(r"\badd\s+rsp,0x([0-9a-f]+)", s)
    if mm:
        rsp += int(mm.group(1), 16)
        print(f"   add rsp,+{int(mm.group(1),16):<4}  rsp now frame+{rsp}")
        pending_lea = None
        continue
    lm = re.search(r"\blea\s+rdi,\[rsp\+0x([0-9a-f]+)\]", s)
    if lm:
        pending_lea = rsp + int(lm.group(1), 16)
        print(f"   lea rdi,[rsp+0x{lm.group(1)}]  -> &frame argument = frame+{pending_lea}")
        continue
    if re.search(r"\bpush\b", s):
        rsp -= 8
        print(f"   push rax/rcx     store lands at frame{rsp:+d}")
        continue
    if re.search(r"\bcall\b", s):
        rsp -= 8
        print(f"   call {s.split()[-1]:<8} return address lands at frame{rsp:+d}")
        continue
    if "iret" in s:
        print("   iretq")
print()