#!/usr/bin/env python3
"""tools/linux-abi/gen_test_elf.py — build the ELF loader's test vectors (GB-013).

Writes deterministic, tiny, licence-free synthetic ELF images into
tests/fixtures/elf/.  They are the T1 (host golden) fixtures for the ELF parser:
one valid static ET_EXEC plus one fixture per rejection class, so the loader's
error paths are tested as hard as its happy path.  Regenerate after changing
gen_test_elf.py; the images are committed so `make host_tests` needs no assembler.

  python3 tools/linux-abi/gen_test_elf.py [outdir]
"""
import os
import struct
import sys

EHDR = struct.Struct("<16sHHIQQQIHHHHHH")   # Elf64_Ehdr
PHDR = struct.Struct("<IIQQQQQQ")           # Elf64_Phdr

ET_EXEC, ET_DYN = 2, 3
EM_X86_64 = 62
PT_LOAD, PT_INTERP, PT_NOTE = 1, 3, 4
PF_X, PF_W, PF_R = 1, 2, 4
BASE = 0x400000
PAGE = 0x1000


def ehdr(etype=ET_EXEC, machine=EM_X86_64, phoff=64, phnum=0, entry=BASE + 0x1000,
         shoff=0, shnum=0, magic=b"\x7fELF", cls=2, data=1, version=1, ehsize=64, phentsize=56):
    # e_ident is 16 bytes: magic, EI_CLASS, EI_DATA, EI_VERSION, EI_OSABI,
    # EI_ABIVERSION, then 7 pad bytes.  Writing only the magic here (and leaving
    # cls/data as dead parameters) silently produced ELFCLASS-none images.
    ident = bytes(magic) + bytes([cls, data, version, 0, 0]) + b"\x00" * 7
    assert len(ident) == 16, len(ident)
    return EHDR.pack(ident, etype, machine, version, entry, phoff, shoff, 0, ehsize,
                     phentsize, phnum, 64, shnum, 0)


def phdr(ptype, flags, off, vaddr, paddr, filesz, memsz, align=PAGE):
    return PHDR.pack(ptype, flags, off, vaddr, paddr, filesz, memsz, align)


def build(name, *, etype=ET_EXEC, machine=EM_X86_64, phnum=None, extra_ph=None,
          segs=None, entry=None, magic=b"\x7fELF", truncate=0, phoff=64,
          cls=2, data=1, extra_hdr_bytes=b""):
    """segs: list of (vaddr_off_from_base, filesz, memsz, flags, fill_byte)."""
    if segs is None:
        segs = [(PAGE, 0x20, 0x40, PF_R | PF_X, 0x90)]         # .text: filesz < memsz -> bss tail
        segs = [(PAGE, 0x20, 0x20, PF_R | PF_X, 0x90),
                (2 * PAGE, 0x10, 0x30, PF_R | PF_W, 0x00)]
    body = bytearray()
    phs = []
    off = PAGE
    for (voff, filesz, memsz, flags, fill) in segs:
        phs.append(phdr(PT_LOAD, flags, off, BASE + voff, BASE + voff, filesz, memsz))
        body += bytes([fill]) * filesz
        pad = (-len(body)) % PAGE
        body += b"\x00" * pad
        off = PAGE + len(body)
    for (ptype, flags, voff, filesz, memsz, fill) in (extra_ph or []):
        phs.append(phdr(ptype, flags, off, BASE + voff, BASE + voff, filesz, memsz))
        body += bytes([fill]) * filesz
        pad = (-len(body)) % PAGE
        body += b"\x00" * pad
    n = phnum if phnum is not None else len(phs)
    e = entry if entry is not None else BASE + PAGE
    img = bytearray(ehdr(etype=etype, machine=machine, phoff=phoff, phnum=n, entry=e,
                         magic=magic, cls=cls, data=data))
    img += extra_hdr_bytes                      # keeps phoff/body alignment when needed
    img += b"".join(phs)
    img += b"\x00" * max(0, PAGE - len(img))
    img += body
    blob = bytes(img)[: len(img) - truncate] if truncate else bytes(img)
    return name, blob


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/elf"
    os.makedirs(outdir, exist_ok=True)
    vectors = [
        build("valid-x86_64-exec"),                                            # accept: 2 PT_LOAD
        build("valid-exec-bss", segs=[(PAGE, 0x20, 0x40, PF_R | PF_X, 0x90)]),  # memsz>filesz -> bss
        build("reject-bad-magic", magic=b"\x7fELg"),                           # not an ELF
        build("reject-arm-machine", machine=183),                              # EM_AARCH64
        build("reject-dyn-pie", etype=ET_DYN),                                 # ET_DYN: Program-2
        build("reject-truncated", truncate=0x1200),                            # cuts into seg1
        build("reject-phoff-oob", phoff=0x10000),                              # phdr table outside file
        build("reject-phnum-zero", phnum=0),                                   # no program headers
        build("reject-interp", extra_ph=[(PT_INTERP, 0, 2 * PAGE, 8, 8, 0x2f)]),  # PT_INTERP -> dynamic
        build("reject-nonload-only", segs=[], extra_ph=[(PT_NOTE, 0, PAGE, 8, 8, 0x00)]),  # no PT_LOAD
        build("reject-class32", cls=1),                                        # ELFCLASS32
        build("reject-bigendian", data=2),                                     # ELFDATA2MSB
        build("reject-filesz-gt-memsz", segs=[(PAGE, 0x40, 0x20, PF_R, 0x90)]),
        build("reject-overlap", segs=[(PAGE, 0x2000, 0x2000, PF_R, 0x11),
                                      (2 * PAGE, 0x10, 0x10, PF_R, 0x22)]),    # overlapping PT_LOADs
        build("reject-bad-entry", entry=BASE + 0x9000),                        # entry outside PT_LOAD
        build("reject-no-exec", segs=[(PAGE, 0x20, 0x20, PF_R | PF_W, 0x90)]),  # nothing mapped PF_X
        build("tour-exec-3seg", segs=[(PAGE, 0x1000, 0x1000, PF_R | PF_X, 0x90),
                                      (2 * PAGE, 0x400, 0x400, PF_R, 0xAB),
                                      (3 * PAGE, 0x80, 0x180, PF_R | PF_W, 0x00)]),
    ]
    names = []
    for name, blob in vectors:
        p = os.path.join(outdir, name + ".elf")
        with open(p, "wb") as f:
            f.write(blob)
        names.append((name, len(blob)))
    for n, sz in names:
        print(f"wrote {outdir}/{n}.elf ({sz} bytes)")
    print(f"{len(names)} vectors")


if __name__ == "__main__":
    main()
