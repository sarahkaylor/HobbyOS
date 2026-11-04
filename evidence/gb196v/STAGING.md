# GB-196-V staging manifest — glibc compat probe

Head: `91933a31` (PR #365). Guest: `MODE=test ARCH=intel`, one-member `::WAVE.CFG`.

## Probe build (host)

```
gcc -pie -fPIE /tmp/ldhello.c -o /tmp/ldhello
readelf -l /tmp/ldhello | grep -i interpreter
  [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]
readelf -d /tmp/ldhello | grep NEEDED
  0x0000000000000001 (NEEDED)  Shared library: [libc.so.6]
```

## Staging (T3_STAGE, after disk.img is built)

```
mcopy -o -i "$ROOT/disk.img" /tmp/ldhello                        ::LDHELLO.BIN
mcopy -o -i "$ROOT/disk.img" /lib64/ld-linux-x86-64.so.2          ::LD64.SO
mcopy -o -i "$ROOT/disk.img" /lib/x86_64-linux-gnu/libc.so.6      ::LIBC6.SO
mcopy -o -i "$ROOT/disk.img" /tmp/ldwave.cfg                      ::WAVE.CFG
```

`::WAVE.CFG` content: one line, `LDHELLO.BIN`.

The no-`LIBC6.SO` boot omits the third mcopy; the pre-change boot uses the same
four files with `src/include/elf_image.h`, `src/kernel/elf_image.c`,
`src/kernel/elf_loader.c` reverted to `138a6874` and rebuilt (`git checkout HEAD`
restores them before the regression runs).
