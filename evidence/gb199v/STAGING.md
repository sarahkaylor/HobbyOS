# GB-199-V staging manifest — ld.so entry probe

Head: `b4f0ac43` (PR #367). Guest: `MODE=test ARCH=intel`, one-member `::WAVE.CFG`.

## Probe build (host)

```
gcc -pie -fPIE /tmp/ldhello.c -o /tmp/ldhello
readelf -h /tmp/ldhello   -> Type: DYN (Position-Independent Executable file)
readelf -l /tmp/ldhello   -> [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]
readelf -d /tmp/ldhello   -> NEEDED libc.so.6
```

## Staging (T3_STAGE, after disk.img is built)

```
mcopy -o -i "$ROOT/disk.img" /tmp/ldhello                     ::LDHELLO.BIN
mcopy -o -i "$ROOT/disk.img" /lib64/ld-linux-x86-64.so.2       ::LD64.SO
mcopy -o -i "$ROOT/disk.img" /lib/x86_64-linux-gnu/libc.so.6   ::LIBC6.SO
mcopy -o -i "$ROOT/disk.img" /lib/x86_64-linux-gnu/libc.so.6   ::libc.so.6
mcopy -o -i "$ROOT/disk.img" /tmp/ldwave.cfg                   ::WAVE.CFG
```

`::WAVE.CFG` content: one line, `LDHELLO.BIN`.

Two staging variants were run because the gate names both `LIBC6.SO` and `libc.so.6`
(real name):

- **alias only** — `::libc.so.6` omitted (`serial_no_lfn.log`): closure prints
  `elf loader: DSO LIBC6.SO mapped`.
- **real name too** — all five files (`serial_normal.log`): mtools stores the real
  name as the LFN `LIBCSO~1.6` (`libc.so.6`), the resolver prefers the exact root
  name, and the closure prints `elf loader: DSO libc.so.6 mapped`. One DSO line
  either way (deduped).

## Falsifier build

```
touch src/kernel/program_loader.c     # force recompile with the flag
make MODE=test ARCH=intel GB199_NO_AUXV=1 disk.img
```

Compile line confirmed `-DGB199_NO_AUXV` (`noauxv_build.log:1`); the same five
files are staged and the same guest booted (`serial_noauxv.log`).
