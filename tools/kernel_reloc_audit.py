#!/usr/bin/env python3
"""kernel_reloc_audit -- find, and gate on, absolute kernel addresses baked
into a linked kernel image's static data.

THE CLASS
---------
`ld.lld` resolves `R_AARCH64_ABS64` in place.  A `const` table of function or
string pointers -- `const struct nv_rm_if x = { "mock", mock_device_open, ... }`
-- therefore ends up in `.rodata`/`.got` holding *link* addresses.  Code
survives the AArch64 image being slid by Limine (`adrp`/`add` are PC-relative);
that data does not.  Calling through such a table branches to a link address
that holds zeros at runtime and the CPU raises ESR EC=0 (undefined encoding) --
an "unknown reason" fault that names nothing.  Round 3's WP-ARM2 report has the
measurement.

WHAT THIS DOES
--------------
Scans every allocated, non-executable PROGBITS section for 8-byte-aligned words
whose value lands inside the kernel's own link range (lowest allocated vaddr ..
end of .bss).  Those are the words a loader would have to relocate.  It then
cross-checks them against the image's `.rela.dyn`:

  * every such word must be covered by an `R_AARCH64_RELATIVE` entry, and
  * `.rela.dyn` must not carry a relocation type the boot pass does not apply.

`arch/arm/reloc.c` applies exactly `R_AARCH64_RELATIVE` at boot, so a word
without one -- or an entry of another type -- is a pointer that will be wrong on
a slid image.  `--gate` turns that into a non-zero exit and the Makefile runs it
on every AArch64 kernel link, so a reintroduced folded table breaks the build
instead of the boot.

THE INVENTORY HALF (a new folded table cannot be added silently)
---------------------------------------------------------------
Relocation coverage alone cannot see a *new* table: while `-pie` is in the link,
every folded word gets its `R_AARCH64_RELATIVE` and the coverage check passes by
construction.  So the gate also compares the image's per-table word counts
against a checked-in inventory (`tools/reloc_tables_baseline.json`, one section
per kernel MODE, audited in `docs/nv-arm-reloc.md`):

  * a table the inventory does not list            -> gate failure (NEW)
  * a listed table that gained word(s)              -> gate failure (GREW)
  * a listed table that lost word(s) or vanished    -> a note, not a failure
  * a MODE with no section of its own               -> checked against the union
                                                       of the audited modes

The two legal responses to a failure are (a) build the table at run time --
member assignment, not a static initializer -- or (b) record the decision:
`--update-baseline` plus the row in `docs/nv-arm-reloc.md`.  `.got` slots are
counted but never gated: the compiler emits them for a function's address in
PIC code, they come and go with ordinary code, and they are not author-written
tables.

ELF metadata that legitimately holds kernel addresses (`.rela*`, `.dynsym`,
`.hash`, `.dynamic`) is skipped: only *program* data is audited.

Exit: 0 clean (or report mode), 1 gate failure, 2 bad usage.

Stdlib only: this runs on a bare build host, no pyelftools.
"""
import argparse
import json
import os
import struct
import sys

SHT_SYMTAB = 2
SHT_RELA = 4
SHT_STRTAB = 3
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHT_NOBITS = 8

R_AARCH64_RELATIVE = 1027
# Types the boot pass applies.  Anything else in .rela.dyn is a build failure:
# an unapplied relocation is exactly the bug this audit exists to prevent.
APPLIED_TYPES = {R_AARCH64_RELATIVE}
TYPE_NAMES = {1025: "R_AARCH64_ABS64", 1026: "R_AARCH64_ABS32",
              1027: "R_AARCH64_RELATIVE", 1028: "R_AARCH64_PREL64"}

# Loader/linker metadata: holds addresses as *metadata*, not as program data.
METADATA = (".rela", ".dynsym", ".gnu.hash", ".hash", ".dynstr",
            ".dynamic", ".interp", ".note")


class Elf:
    def __init__(self, path):
        self.path = path
        d = self.data = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            raise SystemExit(f"{path}: not a 64-bit little-endian ELF")
        self.e_type, self.e_machine = struct.unpack_from("<HH", d, 16)
        (self.e_shoff,) = struct.unpack_from("<Q", d, 40)
        self.e_shentsize, self.e_shnum, self.e_shstrndx = \
            struct.unpack_from("<HHH", d, 58)
        self.sh = [struct.unpack_from("<IIQQQQIIQQ", d,
                                      self.e_shoff + i * self.e_shentsize)
                   for i in range(self.e_shnum)]
        strtab = self.sh[self.e_shstrndx]
        names = d[strtab[4]:strtab[4] + strtab[5]]
        self.names = [names[s[0]:].split(b"\0", 1)[0].decode() for s in self.sh]

    def sections(self):
        return list(zip(self.names, self.sh))

    def body(self, s):
        return b"" if s[1] == SHT_NOBITS else self.data[s[4]:s[4] + s[5]]

    def section(self, name):
        for n, s in self.sections():
            if n == name:
                return s
        return None

    def rela_entries(self):
        """[(section, offset, type, addend)] over every SHT_RELA section."""
        out = []
        for name, s in self.sections():
            if s[1] != SHT_RELA or not s[5]:
                continue
            entsz = s[9] or 24
            blob = self.body(s)
            for off in range(0, len(blob) - 23, entsz):
                r_offset, r_info, r_addend = struct.unpack_from("<QQq", blob, off)
                out.append((name, r_offset, r_info & 0xFFFFFFFF, r_addend))
        return out

    def symbols(self):
        out = []
        for name, s in self.sections():
            if s[1] != SHT_SYMTAB:
                continue
            entsz = s[9] or 24
            tab = self.body(s)
            strsec = self.sh[s[6]]
            strs = self.data[strsec[4]:strsec[4] + strsec[5]]
            for off in range(0, len(tab), entsz):
                st_name, _info, _o, _shndx, st_value, st_size = \
                    struct.unpack_from("<IBBHQQ", tab, off)
                nm = strs[st_name:].split(b"\0", 1)[0].decode(errors="replace")
                if nm:
                    out.append((st_value, st_size, nm))
        out.sort()
        return out


def kernel_range(elf):
    spans = [(s[3], s[3] + s[5]) for _n, s in elf.sections()
             if (s[2] & SHF_ALLOC) and s[5]]
    return min(a for a, _b in spans), max(b for _a, b in spans)


def is_metadata(name):
    return any(name.startswith(m) for m in METADATA)


class Attributor:
    """slot address -> 'symbol+0xoff', else '.section+0xoff'."""

    def __init__(self, elf):
        self.elf = elf
        self.sized = [(v, z, n) for v, z, n in elf.symbols()
                      if z and not n.startswith("$")]
        self.secs = [(n, s[3], s[3] + s[5]) for n, s in elf.sections()
                     if (s[2] & SHF_ALLOC) and s[5]]
        self.ro = elf.section(".rodata")

    def __call__(self, addr):
        for val, size, nm in self.sized:
            if val <= addr < val + size:
                return nm + (f"+0x{addr - val:x}" if addr != val else "")
        for nm, lo, hi in self.secs:
            if lo <= addr < hi:
                off = addr - lo
                s = self.elf.section(nm)
                txt = self._string(s, addr) if nm == ".rodata" else None
                if txt is not None:
                    return f'{nm}+0x{off:x} "{txt}"'
                return f"{nm}+0x{off:x}"
        return "?"

    def _string(self, s, addr):
        """Printable NUL-terminated text at addr, if it looks like a string."""
        if not s:
            return None
        blob = self.elf.body(s)
        off = addr - s[3]
        if off < 0 or off >= len(blob):
            return None
        end = blob.find(b"\0", off, off + 64)
        if end <= off:
            return None
        raw = blob[off:end]
        if all(32 <= c < 127 for c in raw) and len(raw) >= 2:
            return raw.decode("ascii")
        return None


def scan(elf, lo, hi):
    """[(section, slot_addr, value)] for data words holding kernel addresses.

    A non-PIE link leaves the address in the slot; a PIE link leaves the slot
    empty and puts the address in the relocation's addend, so this scan sees
    nothing there (see `relocated()`)."""
    rows = []
    for name, s in elf.sections():
        flags, size, addr = s[2], s[5], s[3]
        if not (flags & SHF_ALLOC) or (flags & SHF_EXECINSTR) or not size:
            continue
        if s[1] == SHT_NOBITS or name.startswith(".bss") or is_metadata(name):
            continue
        blob = elf.body(s)
        for off in range(0, len(blob) - 7, 8):
            v = struct.unpack_from("<Q", blob, off)[0]
            if lo <= v < hi:
                rows.append((name, addr + off, v))
    return rows


def relocated(elf, lo, hi):
    """[(section, slot_addr, addend)] for RELATIVE entries whose addend is an
    in-image address: the pointer words a PIE link leaves to the loader."""
    out = []
    for _sec, off, typ, addend in elf.rela_entries():
        if typ != R_AARCH64_RELATIVE or not (lo <= addend < hi):
            continue
        where = "?"
        for n, s in elf.sections():
            if (s[2] & SHF_ALLOC) and s[5] and s[3] <= off < s[3] + s[5]:
                where = n
                break
        out.append((where, off, addend))
    return out


def table_inventory(rows, attrib):
    """Split the folded population into tables.

    A table is the containing object's symbol name when a sized symbol covers
    the slot (`l_tab_fd`, `reason_names`); a function-local array with no symbol
    of its own lands in its section's bucket (`.rodata`).  Words in
    linker-generated GOT slots are counted apart and never gated: a `.got` entry
    is emitted by the compiler for a function's address in PIC code, it appears
    and disappears with ordinary code, and it is not an author-written table.

    rows: [(section, slot_addr)] -- every folded word in the image.
    Returns ({table: words}, {table: (section, first_slot)}, got_words).
    """
    counts, meta, got = {}, {}, 0
    for sec, slot in sorted(rows, key=lambda r: r[1]):
        if sec.startswith(".got"):
            got += 1
            continue
        key = attrib(slot).split("+0x")[0]
        counts[key] = counts.get(key, 0) + 1
        meta.setdefault(key, (sec, slot))
    return counts, meta, got


def load_baseline(path):
    with open(path) as f:
        data = json.load(f)
    if not isinstance(data, dict) or not isinstance(data.get("modes"), dict):
        raise SystemExit(f"{path}: not an inventory baseline (no 'modes' object)")
    return data


def inventory_ref(data, mode):
    """(ref_tables, scope, exact) for a kernel MODE.

    An audited MODE is compared exactly.  A MODE that has never been audited is
    compared against the union of the audited modes (per table: the largest
    count any of them recorded), which still fails on a table no audit has ever
    seen -- a new one -- while not breaking a tier nobody has measured yet.
    """
    modes = data["modes"]
    if mode in modes:
        return (dict(modes[mode].get("tables", {})), f"MODE={mode}", True)
    ref = {}
    for m in sorted(modes):
        for k, v in modes[m].get("tables", {}).items():
            ref[k] = max(ref.get(k, 0), int(v))
    return ref, "union of " + ", ".join(sorted(modes)), False


def inventory_delta(inv, ref, exact):
    """Compare an image's inventory against the audited one.

    exact=False (an unaudited MODE) only reports additions and growth: a table
    that a different mode does not link is not news.
    """
    new = [(k, inv[k]) for k in sorted(inv) if k not in ref]
    grown = [(k, ref[k], inv[k]) for k in sorted(inv)
             if k in ref and inv[k] > ref[k]]
    shrunk, gone = [], []
    if exact:
        shrunk = [(k, ref[k], inv[k]) for k in sorted(inv)
                  if k in ref and inv[k] < ref[k]]
        gone = sorted(k for k in ref if k not in inv)
    return {"new": new, "grown": grown, "shrunk": shrunk, "gone": gone}


def write_baseline(path, mode, total, inv):
    """Record MODE=mode's inventory, preserving the other modes' sections."""
    try:
        data = load_baseline(path)
    except (OSError, ValueError):
        data = {
            "about": (
                "Folded-table inventory for the AArch64 kernel image.  Every "
                "entry is a table of function/string pointers that the compiler "
                "folded into static data and ld.lld resolved to LINK addresses; "
                "arch/arm/reloc.c applies one R_AARCH64_RELATIVE per word at "
                "boot.  tools/kernel_reloc_audit.py --gate fails the link when a "
                "table appears (NEW) or grows (GREW) without this file and "
                "docs/nv-arm-reloc.md being updated on purpose.  Regenerate with "
                "--update-baseline; do not hand-edit."),
            "generated_by": "tools/kernel_reloc_audit.py --update-baseline",
            "modes": {},
        }
    old = dict(data["modes"].get(mode, {}).get("tables", {}))
    delta = inventory_delta(inv, old, exact=True)
    data["modes"][mode] = {
        "total": total,
        "tables": dict(sorted(inv.items(), key=lambda kv: (-kv[1], kv[0]))),
    }
    with open(path, "w") as f:
        json.dump(data, f, indent=1)
        f.write("\n")
    print(f"{path}: MODE={mode} inventory recorded -- {len(inv)} table(s), "
          f"{total} word(s)")
    for k, w in delta["new"]:
        print(f"  + {k} = {w}")
    for k, a, b in delta["grown"]:
        print(f"  ^ {k} {a} -> {b}")
    for k, a, b in delta["shrunk"]:
        print(f"  v {k} {a} -> {b}")
    for k in delta["gone"]:
        print(f"  - {k}")


def group(rows):
    """Fold adjacent slots into runs (one run == one table, usually)."""
    rows = sorted(rows, key=lambda r: r[1])
    out, cur = [], []
    for r in rows:
        if cur and r[1] - cur[-1][1] <= 8:
            cur.append(r)
        else:
            if cur:
                out.append(cur)
            cur = [r]
    if cur:
        out.append(cur)
    return out


BASELINE_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "reloc_tables_baseline.json")


def main(argv):
    ap = argparse.ArgumentParser(
        description="audit/gate kernel static data for unrelocated pointers")
    ap.add_argument("elf")
    ap.add_argument("--gate", action="store_true",
                    help="exit 1 when a kernel pointer has no applied relocation "
                         "or a folded table is not in the audited inventory")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--runs", action="store_true",
                    help="group adjacent slots into table runs")
    ap.add_argument("--by-symbol", action="store_true",
                    help="summarise pointer words per containing table")
    ap.add_argument("--baseline", default=BASELINE_DEFAULT,
                    help="folded-table inventory to gate against "
                         "(default: tools/reloc_tables_baseline.json)")
    ap.add_argument("--mode", default=None,
                    help="kernel MODE of the image; selects the inventory "
                         "section (pass $(MODE) from the Makefile)")
    ap.add_argument("--no-inventory", action="store_true",
                    help="gate on relocation coverage only -- for auditing an "
                         "image this tree's inventory does not describe")
    ap.add_argument("--update-baseline", action="store_true",
                    help="record this image's inventory for --mode in --baseline")
    ap.add_argument("--max-rows", type=int, default=40)
    args = ap.parse_args(argv[1:])

    if (args.update_baseline or (args.gate and not args.no_inventory)) \
            and not args.mode:
        ap.error("--mode is required with --gate/--update-baseline: the "
                 "inventory is kept per kernel MODE")

    elf = Elf(args.elf)
    lo, hi = kernel_range(elf)
    baked = scan(elf, lo, hi)
    relocs = elf.rela_entries()
    covered = {off for _s, off, typ, _a in relocs if typ in APPLIED_TYPES}
    uncovered = [r for r in baked if r[1] not in covered]
    pointers = relocated(elf, lo, hi)
    bad_types = sorted({typ for _s, _o, typ, _a in relocs
                        if typ not in APPLIED_TYPES})
    has_rela = elf.section(".rela.dyn") is not None
    total = len(uncovered) + len(pointers)
    attrib = Attributor(elf)
    inv, inv_meta, got_words = table_inventory(
        [(sec, slot) for sec, slot, _a in pointers]
        + [(sec, slot) for sec, slot, _v in uncovered], attrib)

    if args.update_baseline:
        write_baseline(args.baseline, args.mode, total, inv)

    if args.json:
        print(json.dumps({
            "elf": args.elf,
            "range": [lo, hi],
            "pointer_words": total,
            "relocated": len(pointers),
            "unapplied": [{"slot": r[1], "value": r[2]} for r in uncovered],
            "rela_dyn": len(relocs),
            "bad_types": bad_types,
            "tables": inv,
            "got": got_words,
        }, indent=1))
        return 0 if (not args.gate or (not uncovered and not bad_types)) else 1

    print(f"# {args.elf}")
    print(f"# kernel link range [0x{lo:X}, 0x{hi:X})   .rela.dyn="
          f"{len(relocs)} entries ({'present' if has_rela else 'ABSENT'})")
    print(f"# pointer words baked into static data (need relocating): {total}")
    print(f"#   applied at boot (R_AARCH64_RELATIVE, addend in image): {len(pointers)}")
    print(f"#   NOTHING WILL APPLY THEM (left at their link address):     {len(uncovered)}")
    print(f"# folded-table inventory: {len(inv)} table(s) / "
          f"{sum(inv.values())} word(s), plus {got_words} linker-generated "
          f".got slot(s)")
    if args.by_symbol:
        per = {}
        for sec, slot, val in pointers + uncovered:
            name = attrib(slot).split("+0x")[0]
            row = per.setdefault(name, {"words": 0, "sample_target": attrib(val),
                                        "section": sec, "first": slot})
            row["words"] += 1
        print(f"\n{'words':>5}  {'first slot':<16} {'section':<9} table"
              f"{'':<30} -> first target")
        for name, r in sorted(per.items(), key=lambda kv: -kv[1]["words"]):
            print(f"{r['words']:>5}  0x{r['first']:012X} {r['section']:<9} "
                  f"{name:<38} -> {r['sample_target']}")
    if args.runs:
        print()
        for run in group(pointers + [r for r in uncovered]):
            s, e = run[0][1], run[-1][1]
            print(f"  {run[0][0]:<9} 0x{s:012X}..0x{e:012X} {len(run):>4} word(s)"
                  f"  in {attrib(s):<38} -> {attrib(run[0][2])}")
    elif uncovered:
        for r in uncovered[:args.max_rows]:
            print(f"  UNAPPLIED 0x{r[1]:012X} = 0x{r[2]:012X}  "
                  f"in {attrib(r[1])} -> {attrib(r[2])}")
        if len(uncovered) > args.max_rows:
            print(f"  ... {len(uncovered) - args.max_rows} more")
    if bad_types:
        print("# UNHANDLED relocation type(s): "
              + ", ".join(TYPE_NAMES.get(t, hex(t)) for t in bad_types))

    if not args.gate:
        return 0

    ok = not uncovered and not bad_types
    scope = ""
    delta = None
    if not args.no_inventory:
        try:
            base = load_baseline(args.baseline)
        except OSError as e:
            print(f"{args.elf}: FAIL -- cannot read the folded-table inventory "
                  f"({e}).  It is checked in so a new table cannot appear "
                  f"silently; regenerate it with --update-baseline --mode "
                  f"{args.mode}.", file=sys.stderr)
            return 2
        ref, scope, exact = inventory_ref(base, args.mode)
        delta = inventory_delta(inv, ref, exact)
        if delta["new"] or delta["grown"]:
            ok = False

    if delta:
        for k, a, b in delta["shrunk"]:
            print(f"  note: {k} shrank {a} -> {b} word(s) -- refresh the "
                  f"inventory with --update-baseline when convenient")
        for k in delta["gone"]:
            print(f"  note: {k} is listed but not in this image -- refresh the "
                  f"inventory with --update-baseline when convenient")

    if ok:
        print(f"{args.elf}: reloc gate OK -- {total} kernel pointer word(s), "
              f"every one applied at boot")
        if delta is not None:
            print(f"{args.elf}: folded-table inventory matches -- {len(inv)} "
                  f"table(s) / {sum(inv.values())} word(s) as audited [{scope}]")
        return 0

    if uncovered or bad_types:
        sys.stdout.flush()
        print(f"{args.elf}: FAIL -- {len(uncovered)} kernel pointer(s) with no "
              f"applied relocation, {len(bad_types)} unhandled relocation "
              f"type(s). A slid AArch64 image cannot execute a pointer left at "
              f"its link address: build the table at runtime (member assignment, "
              f"not a static initializer) or store it as fixed-size inline "
              f"arrays.", file=sys.stderr)
    if delta and (delta["new"] or delta["grown"]):
        sys.stdout.flush()
        print(f"{args.elf}: FAIL -- folded table(s) not in the audited "
              f"inventory [{scope}]:", file=sys.stderr)
        for k, w in delta["new"]:
            sec, slot = inv_meta.get(k, ("?", 0))
            print(f"  NEW   {k:<34} {w:>4} word(s)  {sec}  first slot "
                  f"0x{slot:012X}  {attrib(slot)}", file=sys.stderr)
        for k, a, b in delta["grown"]:
            sec, slot = inv_meta.get(k, ("?", 0))
            print(f"  GREW  {k:<34} {a} -> {b} word(s)  {sec}  first slot "
                  f"0x{slot:012X}  {attrib(slot)}", file=sys.stderr)
        print("A folded const char*/function-pointer table is a link-address "
              "hazard: every word in it needs an R_AARCH64_RELATIVE at boot.  "
              "Two responses are legal:\n"
              "  (a) build it at run time -- member assignment, not a static "
              "initializer (src/kernel/nv_rm_mock.c is the worked example); or\n"
              "  (b) keep it and record the decision: "
              f"python3 tools/kernel_reloc_audit.py --update-baseline "
              f"--mode {args.mode} {args.elf}\n"
              "      plus its row in docs/nv-arm-reloc.md (the audit list).",
              file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
