import re, glob

for f in sorted(glob.glob("evidence/gb-101/run*.serial.log")):
    txt = open(f, "rb").read().decode("latin-1")
    fatal = re.findall(r"FATAL kernel-mode fault, halting this CPU: vector=(\d+) rip=0x([0-9A-Fa-f]+)", txt)
    exc = re.findall(r"FATAL: Exception in Kernel Mode! Vector: (\d+)", txt)
    weird = [v for v, r in fatal if int(v) not in list(range(0, 32))]
    lowrip = [r for v, r in fatal if int(r, 16) < 0x1000]
    print(f"{f}: intact-FATAL-lines={len(fatal)} exc-banner={len(exc)} "
          f"impossible-vector={weird} rip<0x1000={lowrip}")
    if fatal:
        for v, r in fatal[:4]:
            print(f"    vector={v} rip=0x{r}")