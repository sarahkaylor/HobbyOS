#!/usr/bin/env python3
"""tools/cp/p0host/pe.py <exe> — PE header facts HobbyOS needs (read-only).

ImageBase, SizeOfImage, DllCharacteristics flags, subsystem, section table,
TLS callbacks, imports by DLL, and the file version resource.  Prints text only.
"""
import sys, pefile, hashlib, os

p = sys.argv[1]
pe = pefile.PE(p, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY[d] for d in (
    "IMAGE_DIRECTORY_ENTRY_IMPORT", "IMAGE_DIRECTORY_ENTRY_TLS", "IMAGE_DIRECTORY_ENTRY_RESOURCE",
    "IMAGE_DIRECTORY_ENTRY_BASERELOC", "IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT")])
oh, fh = pe.OPTIONAL_HEADER, pe.FILE_HEADER
print("file", os.path.basename(p), "size", os.path.getsize(p))
print("Machine 0x%04x  Characteristics 0x%04x" % (fh.Machine, fh.Characteristics))
print("Magic 0x%x  ImageBase 0x%x  SizeOfImage 0x%x (%d MiB)  SectionAlignment 0x%x" %
      (oh.Magic, oh.ImageBase, oh.SizeOfImage, oh.SizeOfImage >> 20, oh.SectionAlignment))
print("Subsystem %d (%s)  MajorSubsystemVersion %d.%d" % (oh.Subsystem,
      pefile.SUBSYSTEM_TYPE.get(oh.Subsystem), oh.MajorSubsystemVersion, oh.MinorSubsystemVersion))
dc = oh.DllCharacteristics
flags = [n for n, v in pefile.dll_characteristics if dc & v]
print("DllCharacteristics 0x%04x %s" % (dc, flags))
print("SizeOfStackReserve 0x%x SizeOfStackCommit 0x%x SizeOfHeapReserve 0x%x" %
      (oh.SizeOfStackReserve, oh.SizeOfStackCommit, oh.SizeOfHeapReserve))
print("relocations present:", hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"),
      "(%d blocks)" % len(getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])))
print("sections:")
for s in pe.sections:
    print("  %-8s va 0x%08x vsize 0x%08x raw 0x%08x chars 0x%08x" % (
        s.Name.rstrip(b"\0").decode(errors="replace"), s.VirtualAddress, s.Misc_VirtualSize,
        s.SizeOfRawData, s.Characteristics))
if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
    t = pe.DIRECTORY_ENTRY_TLS.struct
    print("TLS: StartRaw 0x%x EndRaw 0x%x Callbacks@0x%x SizeOfZeroFill %d" %
          (t.StartAddressOfRawData, t.EndAddressOfRawData, t.AddressOfCallBacks, t.SizeOfZeroFill))
print("imports:")
for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    print("  %-40s %4d" % (e.dll.decode(), len(e.imports)))
for e in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
    print("  (delay) %-32s %4d" % (e.dll.decode(), len(e.imports)))
try:
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    for fi in getattr(pe, "FileInfo", []):
        for x in fi:
            for st in getattr(x, "StringTable", []):
                for k, v in st.entries.items():
                    if k in (b"FileVersion", b"ProductVersion", b"ProductName", b"CompanyName"):
                        print("version:", k.decode(), "=", v.decode())
except Exception as ex:
    print("version resource: error", ex)
h = hashlib.sha256(open(p, "rb").read()).hexdigest()
print("sha256", h)
