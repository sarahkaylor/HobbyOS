/*
 * prl_probe.c - an aarch64 UEFI hardware probe for a Parallels ARM VM.
 *
 * Why this exists: a Parallels Desktop ARM guest gives HobbyOS no serial port,
 * no virtio-mmio disk and no QEMU chipset, so a straight boot produces a black
 * screen and nothing else.  This app runs as EFI/BOOT/BOOTAA64.EFI and reports
 * what the firmware actually exposes, so the port is driven by measurements
 * instead of guesses:
 *
 *   - GOP: framebuffer base/size, resolution, pitch, pixel format, all modes
 *   - ACPI: RSDP/XSDT and every table signature (MCFG -> ECAM, SPCR -> serial)
 *   - BlockIo: every disk, block size, last LBA, and whether it is read-only
 *   - SimpleFileSystem: volumes, their attributes, and a real write test
 *   - PCI: ECAM scan, vendor:device, class, BARs, bridges
 *   - memory map totals in address order
 *
 * Output channels, in order of reliability, because there is no serial port:
 *   1. \PROBE.TXT on the first writable FAT volume (read back with mtype);
 *   2. raw sectors at the end of that same disk, magic "HBPROBE1", written
 *      through EFI_BLOCK_IO_PROTOCOL, which works even when the filesystem
 *      layer refuses (read back with dd);
 *   3. ConOut, 78 columns, so the VM window itself is legible.
 *
 * Build: tools/probe/build.sh   (clang + lld-link, PE32+ aarch64 EFI app)
 */

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef u64                UINTN;
typedef u64                EFI_PHYSICAL_ADDRESS;
typedef u64                EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef u16                CHAR16;

#define EFIAPI
#define EFI_SUCCESS 0
#define EFI_ERROR(s) (((long long)(s)) < 0)

#define LINE_MAX 90

typedef struct { u64 Signature; u32 Revision; u32 HeaderSize; u32 CRC32; u32 Reserved; } EFI_TABLE_HEADER;

typedef struct { u8 Guid[16]; void *Data; } EFI_CONFIGURATION_TABLE;

typedef struct {
    u32 Type;
    u32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_PHYSICAL_ADDRESS VirtualStart;
    u64 NumberOfPages;
    u64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (*OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
    void *TestString;
    void *QueryMode;
    EFI_STATUS (*SetMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    u32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    struct EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (*AllocatePages)(u32, u32, UINTN, EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
    EFI_STATUS (*GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, u32 *);
    EFI_STATUS (*AllocatePool)(u32, UINTN, void **);
    EFI_STATUS (*FreePool)(void *);
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE, void *, void **);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STATUS (*Stall)(UINTN);
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    EFI_STATUS (*OpenProtocol)(EFI_HANDLE, void *, void **, EFI_HANDLE, EFI_HANDLE, u32);
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (*LocateHandleBuffer)(u32, void *, void *, UINTN *, EFI_HANDLE **);
    EFI_STATUS (*LocateProtocol)(void *, void *, void **);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct { u32 RedMask, GreenMask, BlueMask, ReservedMask; } EFI_PIXEL_BITMASK;

typedef struct {
    u32 Version;
    u32 HorizontalResolution;
    u32 VerticalResolution;
    u32 PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    u32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    u32 MaxMode;
    u32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    EFI_STATUS (*QueryMode)(void *, u32, UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (*SetMode)(void *, u32);
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    u32 MediaId;
    u8  RemovableMedia;
    u8  MediaPresent;
    u8  LogicalPartition;
    u8  ReadOnly;
    u8  WriteCaching;
    u8  Pad[3];
    u32 BlockSize;
    u32 IoAlign;
    u64 LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct {
    u64 Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_STATUS (*Reset)(void *, u8);
    EFI_STATUS (*ReadBlocks)(void *, u32, u64, UINTN, void *);
    EFI_STATUS (*WriteBlocks)(void *, u32, u64, UINTN, void *);
    EFI_STATUS (*FlushBlocks)(void *);
} EFI_BLOCK_IO_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    u64 Revision;
    EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **, CHAR16 *, u64, u64);
    EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);
    void *Delete;
    EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS (*Write)(EFI_FILE_PROTOCOL *, UINTN *, void *);
    void *GetPosition;
    void *SetPosition;
    EFI_STATUS (*GetInfo)(EFI_FILE_PROTOCOL *, void *, UINTN *);
    void *SetInfo;
    EFI_STATUS (*Flush)(EFI_FILE_PROTOCOL *);
};

typedef struct {
    u64 Revision;
    EFI_STATUS (*OpenVolume)(void *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

static const u8 GUID_GRAPHICS_OUTPUT[16] = {0xde,0xa9,0x42,0x90,0xdc,0x23,0x38,0x4a,
                                            0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a};
static const u8 GUID_SIMPLE_FS[16]       = {0x22,0x5b,0x4e,0x96,0x59,0x64,0xd2,0x11,
                                            0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b};
static const u8 GUID_BLOCK_IO[16]        = {0x21,0x5b,0x4e,0x96,0x59,0x64,0xd2,0x11,
                                            0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b};
static const u8 GUID_FILE_INFO[16]       = {0x92,0x6e,0x57,0x09,0x3f,0x6d,0xd2,0x11,
                                            0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b};
static const u8 GUID_ACPI_20[16]         = {0x71,0xe8,0x68,0x88,0xf1,0xe4,0xd3,0x11,
                                            0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81};
static const u8 GUID_ACPI_10[16]         = {0x30,0x2d,0x9d,0xeb,0x88,0x2d,0xd3,0x11,
                                            0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d};

/* ------------------------- line-oriented text buffer ---------------------- */
static char out_buf[40 * 1024];
static u32  out_len;
static u32  line_len;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out;

static void out_flush_screen(void);

static void putc_(char c) {
    if (out_len < sizeof(out_buf) - 1) out_buf[out_len++] = c;
    if (c == '\n') line_len = 0; else line_len++;
}

static void puts_(const char *s) { while (*s) putc_(*s++); }
static void hex_(u64 v) {
    char t[18]; int n = 0; t[n++] = '0'; t[n++] = 'x';
    if (!v) { t[n++] = '0'; }
    else { int st = 0; for (int i = 60; i >= 0; i -= 4) { u32 d = (u32)((v >> i) & 0xf);
            if (d || st) { st = 1; t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); } } }
    t[n] = 0; puts_(t);
}
static void dec_(u64 v) {
    char t[24]; int n = 0;
    if (!v) { putc_('0'); return; }
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) putc_(t[--n]);
}
static void sdec_(long long v) { if (v < 0) { putc_('-'); dec_((u64)(-v)); } else dec_((u64)v); }
static void nl_(void) { putc_('\r'); putc_('\n'); }

/* start a new line if the current one would overflow the 80-col console */
static void wrap_check(const char *next) {
    u32 n = 0;
    while (next[n]) n++;
    if (line_len + n > 78) nl_();
}

static void kv_(const char *k, u64 v) { wrap_check(k); puts_(k); hex_(v); }
static void kv_end(void) { nl_(); }

/* ------------------------------- output sinks ----------------------------- */
static void out_flush_screen(void) {
    if (!con_out) return;
    u32 i = 0;
    while (i < out_len) {
        CHAR16 line[LINE_MAX];
        u32 n = 0;
        while (i < out_len && n < LINE_MAX - 1) {
            char c = out_buf[i++];
            line[n++] = (CHAR16)(u8)c;
            if (c == '\n') break;
        }
        line[n] = 0;
        con_out->OutputString(con_out, line);
    }
}

/* ---------------------------- SimpleFileSystem ---------------------------- */
static EFI_STATUS try_write_file(EFI_SYSTEM_TABLE *st, const char *tag) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    UINTN count = 0;
    EFI_HANDLE *handles = 0;
    if (EFI_ERROR(bs->LocateHandleBuffer(2, (void *)GUID_SIMPLE_FS, 0, &count, &handles))) return 1;
    puts_("[fs] volumes: "); dec_(count); nl_();

    EFI_STATUS result = 2;
    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
        if (EFI_ERROR(bs->HandleProtocol(handles[i], (void *)GUID_SIMPLE_FS, (void **)&fs)) || !fs) continue;
        EFI_FILE_PROTOCOL *root = 0;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        /* volume attributes: bit 0x01 is EFI_FILE_READ_ONLY */
        u8 info[512];
        UINTN ilen = sizeof(info);
        EFI_STATUS gi = root->GetInfo(root, (void *)GUID_FILE_INFO, &ilen);
        puts_("[fs] vol "); dec_(i);
        if (!EFI_ERROR(gi)) {
            u64 attr = *(u64 *)(info + 72);
            kv_(" attr=", attr); kv_(" size=", *(u64 *)(info + 8));
        } else { kv_(" getinfo=", gi); }
        nl_();

        static CHAR16 name[] = {'\\', 'P', 'R', 'O', 'B', 'E', '.', 'T', 'X', 'T', 0};
        EFI_FILE_PROTOCOL *f = 0;
        EFI_STATUS s = root->Open(root, &f, name, 0x00000001 | 0x00000002 | 0x00000008, 0x20);
        kv_("[fs] open=", s); nl_();
        if (EFI_ERROR(s) || !f) { root->Close(root); continue; }

        puts_(tag); nl_();
        UINTN len = out_len;
        s = f->Write(f, &len, out_buf);
        kv_("[fs] write=", s); kv_(" wrote=", len); nl_();
        f->Flush(f);
        f->Close(f);
        root->Close(root);
        if (!EFI_ERROR(s)) result = 0;
        break;
    }
    return result;
}

/* Raw sector dump through BlockIo: survives a read-only or absent filesystem. */
#define RAW_MAGIC "HBPROBE1"
#define RAW_SECTORS 96
static EFI_STATUS raw_dump(EFI_SYSTEM_TABLE *st) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    UINTN count = 0;
    EFI_HANDLE *handles = 0;
    if (EFI_ERROR(bs->LocateHandleBuffer(2, (void *)GUID_BLOCK_IO, 0, &count, &handles))) {
        puts_("[raw] no BlockIo handles\n"); return 1;
    }
    EFI_STATUS last = 1;
    for (UINTN i = 0; i < count && i < 8; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = 0;
        if (EFI_ERROR(bs->HandleProtocol(handles[i], (void *)GUID_BLOCK_IO, (void **)&bio)) || !bio) continue;
        EFI_BLOCK_IO_MEDIA *m = bio->Media;
        if (!m || !m->MediaPresent || !m->LastBlock) continue;
        if (m->BlockSize != 512) continue;
        u64 bytes = (u64)RAW_SECTORS * 512;
        if (m->LastBlock < RAW_SECTORS + 64) continue;
        u64 lba = m->LastBlock - RAW_SECTORS;

        u8 *buf = 0;
        if (EFI_ERROR(bs->AllocatePool(2, bytes, (void **)&buf)) || !buf) continue;
        for (u64 k = 0; k < bytes; k++) buf[k] = 0;
        for (int k = 0; k < 8; k++) buf[k] = (u8)RAW_MAGIC[k];
        u32 n = out_len < bytes - 16 ? out_len : (u32)(bytes - 16);
        for (u32 k = 0; k < n; k++) buf[8 + k] = (u8)out_buf[k];

        EFI_STATUS s = bio->WriteBlocks(bio, m->MediaId, lba, bytes, buf);
        kv_("[raw] lba=", lba); kv_(" write=", s); nl_();
        bs->FreePool(buf);
        last = s;
        break;
    }
    return last;
}

/* ------------------------------- ACPI ------------------------------------ */
static u8 *find_rsdp(EFI_SYSTEM_TABLE *st) {
    u8 *rsdp = 0;
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &st->ConfigurationTable[i];
        int m20 = 1;
        for (int b = 0; b < 16; b++) if (ct->Guid[b] != GUID_ACPI_20[b]) m20 = 0;
        if (m20) return (u8 *)ct->Data;
        int m10 = 1;
        for (int b = 0; b < 16; b++) if (ct->Guid[b] != GUID_ACPI_10[b]) m10 = 0;
        if (m10) rsdp = (u8 *)ct->Data;
    }
    return rsdp;
}

static u64 g_ecam_base;
static u8  g_bus_start, g_bus_end;

static void dump_acpi(EFI_SYSTEM_TABLE *st) {
    puts_("[acpi] tables: "); dec_(st->NumberOfTableEntries); nl_();
    u8 *rsdp = find_rsdp(st);
    if (!rsdp) { puts_("[acpi] no RSDP\n"); return; }
    kv_("[acpi] rsdp=", (u64)(UINTN)rsdp);
    puts_(" rev="); dec_(rsdp[15]); nl_();
    u64 xsdt = *(u64 *)(rsdp + 24);
    u64 rsdt = *(u32 *)(rsdp + 16);
    u64 sdt = xsdt ? xsdt : rsdt;
    kv_("[acpi] xsdt=", xsdt); nl_();
    if (!sdt) return;
    u8 *hdr = (u8 *)(UINTN)sdt;
    u32 len = *(u32 *)(hdr + 4);
    u32 entries = (len - 36) / 8;
    for (u32 i = 0; i < entries && i < 40; i++) {
        u64 t = *(u64 *)(hdr + 36 + i * 8);
        if (!t) continue;
        u8 *p = (u8 *)(UINTN)t;
        u32 tlen = *(u32 *)(p + 4);
        puts_("  ");
        for (int k = 0; k < 4; k++) putc_((char)(p[k] ? p[k] : '?'));
        puts_(" len="); dec_(tlen);
        if (p[0]=='M'&&p[1]=='C'&&p[2]=='F'&&p[3]=='G') {
            g_ecam_base = *(u64 *)(p + 44);
            g_bus_start = p[54]; g_bus_end = p[55];
            puts_(" ECAM="); hex_(g_ecam_base);
            puts_(" bus="); dec_(g_bus_start); putc_('-'); dec_(g_bus_end);
        }
        if (p[0]=='S'&&p[1]=='P'&&p[2]=='C'&&p[3]=='R') {
            puts_(" SERIAL space="); dec_(p[36]);
            puts_(" base="); hex_(*(u64 *)(p + 40));
        }
        if (p[0]=='A'&&p[1]=='P'&&p[2]=='I'&&p[3]=='C') {
            u32 ml = *(u32 *)(p + 4);
            u32 o = 44;
            while (o + 2 <= ml) {
                u8 type = p[o], elen = p[o + 1];
                if (!elen) break;
                if (type == 0x0b) {   /* GICC: CPU interface (GICv2 style) */
                    puts_(" GICC cpuif="); hex_(*(u32 *)(p + o + 4));
                }
                if (type == 0x0c) {   /* GICD */
                    puts_(" GICD="); hex_(*(u32 *)(p + o + 8));
                }
                if (type == 0x0e) {   /* GICR */
                    puts_(" GICR="); hex_(*(u64 *)(p + o + 4));
                }
                o += elen;
            }
        }
        if (p[0]=='G'&&p[1]=='T'&&p[2]=='D'&&p[3]=='T')
            { puts_(" cntctl="); hex_(*(u64 *)(p + 48)); }
        if (p[0]=='F'&&p[1]=='A'&&p[2]=='C'&&p[3]=='P')
            { puts_(" legacy-free="); dec_(p[109]); }
        nl_();
    }
}

/* -------------------------------- PCI ------------------------------------ */
static u32 pci_rd(u8 bus, u8 dev, u8 fn, u16 off) {
    if (!g_ecam_base) return 0xffffffffu;
    volatile u32 *p = (volatile u32 *)(UINTN)(g_ecam_base + ((u64)bus << 20) +
                                              ((u64)dev << 15) + ((u64)fn << 12) + (off & 0xffc));
    return *p;
}

static void dump_pci(void) {
    kv_("[pci] ecam=", g_ecam_base); nl_();
    if (!g_ecam_base) { puts_("[pci] no MCFG\n"); return; }
    int found = 0;
    u8 top = g_bus_end > 7 ? 7 : g_bus_end;
    for (u32 bus = g_bus_start; bus <= top; bus++) {
        for (u32 dev = 0; dev < 32; dev++) {
            for (u32 fn = 0; fn < 8; fn++) {
                u32 id = pci_rd((u8)bus, (u8)dev, (u8)fn, 0);
                if ((id & 0xffff) == 0xffff || (id & 0xffff) == 0) continue;
                found++;
                u32 cls = pci_rd((u8)bus, (u8)dev, (u8)fn, 8);
                u32 hd  = pci_rd((u8)bus, (u8)dev, (u8)fn, 0xc);
                u8  ht  = (u8)((hd >> 16) & 0xff);
                puts_("  "); dec_(bus); putc_(':'); dec_(dev); putc_('.');
                dec_(fn); puts_(" ");
                hex_(id & 0xffff); putc_(':'); hex_(id >> 16);
                puts_(" c"); hex_(cls >> 24); putc_('/');
                hex_((cls >> 16) & 0xff); putc_('/'); hex_((cls >> 8) & 0xff);
                if (ht == 1) {
                    u32 sb = pci_rd((u8)bus, (u8)dev, (u8)fn, 0x18);
                    puts_(" br"); dec_((sb >> 8) & 0xff); putc_('-'); dec_((sb >> 16) & 0xff);
                } else {
                    for (int b = 0; b < 2; b++) {
                        u32 bar = pci_rd((u8)bus, (u8)dev, (u8)fn, (u16)(0x10 + b * 4));
                        if (bar && bar != 0xffffffff) { putc_(' '); hex_(bar); }
                    }
                }
                nl_();
                if (found > 40) { puts_("  ...\n"); return; }
            }
        }
    }
    if (!found) puts_("  none\n");
}

/* Dump the 64-byte config header of each device: BARs, IRQ, and the
 * programming interface tell a driver author what the controller expects. */
static void dump_pci_detail(void) {
    if (!g_ecam_base) return;
    for (u8 dev = 0; dev < 32; dev++) {
        for (u8 fn = 0; fn < 8; fn++) {
            u32 id = pci_rd(0, dev, fn, 0);
            if ((id & 0xffff) == 0xffff || (id & 0xffff) == 0) continue;
            puts_("  d"); dec_(dev); putc_('.'); dec_(fn); puts_(":");
            for (u16 off = 0; off < 64; off += 4) { putc_(' '); hex_(pci_rd(0, dev, fn, off)); }
            nl_();
        }
    }
}

/* ------------------------------ memory map -------------------------------- */
static void dump_memmap(EFI_SYSTEM_TABLE *st) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    UINTN size = 0, key = 0, dsz = 0; u32 ver = 0;
    bs->GetMemoryMap(&size, 0, &key, &dsz, &ver);
    size += 8 * dsz;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    if (EFI_ERROR(bs->AllocatePool(2, size, (void **)&map)) || !map) { puts_("[mem] pool fail\n"); return; }
    if (EFI_ERROR(bs->GetMemoryMap(&size, map, &key, &dsz, &ver))) { puts_("[mem] failed\n"); bs->FreePool(map); return; }
    UINTN n = size / dsz;
    u64 ram = 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((u8 *)map + i * dsz);
        if (d->Type == 4 || d->Type == 7) ram += d->NumberOfPages * 4096;
    }
    puts_("[mem] regions="); dec_(n); puts_(" ram="); dec_(ram >> 20); puts_("MiB\n");
    for (UINTN i = 0; i < n && i < 40; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((u8 *)map + i * dsz);
        puts_("  "); hex_(d->PhysicalStart); putc_('+'); hex_(d->NumberOfPages * 4096);
        puts_(" t"); dec_(d->Type); puts_(" a"); hex_(d->Attribute); nl_();
    }
    bs->FreePool(map);
}

/* Scan the firmware's MMIO regions for a virtio-mmio transport: magic "virt"
 * at offset 0 and a device id at 8.  Apple's hypervisor and QEMU both expose
 * virtio this way, and HobbyOS already speaks exactly this transport. */
static void scan_virtio_mmio(EFI_SYSTEM_TABLE *st) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    UINTN size = 0, key = 0, dsz = 0; u32 ver = 0;
    bs->GetMemoryMap(&size, 0, &key, &dsz, &ver);
    size += 8 * dsz;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    if (EFI_ERROR(bs->AllocatePool(2, size, (void **)&map)) || !map) return;
    if (EFI_ERROR(bs->GetMemoryMap(&size, map, &key, &dsz, &ver))) { bs->FreePool(map); return; }
    UINTN n = size / dsz;
    int hits = 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((u8 *)map + i * dsz);
        if (d->Type != 11 && d->Type != 12) continue;     /* MMIO / MMIO port */
        u64 base = d->PhysicalStart, len = d->NumberOfPages * 4096;
        if (len > 0x4000000ull) continue;                /* skip giant apertures */
        for (u64 off = 0; off + 0x100 <= len; off += 4096) {
            volatile u32 *m = (volatile u32 *)(UINTN)(base + off);
            if (m[0] != 0x74726976u) continue;           /* "virt" */
            hits++;
            puts_("  virtio-mmio "); hex_(base + off);
            puts_(" dev="); dec_(m[2] & 0xff);
            puts_(" ver="); dec_(m[1]);
            nl_();
        }
    }
    puts_("[virtio] mmio devices: "); dec_(hits); nl_();
    bs->FreePool(map);
}

/* -------------------------------- GOP ------------------------------------ */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *g_gop;

static void dump_gop(EFI_SYSTEM_TABLE *st) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (EFI_ERROR(st->BootServices->LocateProtocol((void *)GUID_GRAPHICS_OUTPUT, 0, (void **)&gop)) || !gop) {
        puts_("[gop] absent\n"); return;
    }
    g_gop = gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *m = gop->Mode;
    kv_("[gop] fb=", m->FrameBufferBase);
    kv_(" size=", m->FrameBufferSize >> 20); puts_("MiB\n");
    kv_("[gop] maxmode=", m->MaxMode); kv_(" cur=", m->Mode); nl_();
    if (m->Info) {
        puts_("[gop] "); dec_(m->Info->HorizontalResolution); putc_('x');
        dec_(m->Info->VerticalResolution); puts_(" pitch="); dec_(m->Info->PixelsPerScanLine);
        puts_(" fmt="); dec_(m->Info->PixelFormat); nl_();
    }
    for (u32 mode = 0; mode < m->MaxMode && mode < 8; mode++) {
        UINTN isz = 0; EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = 0;
        if (EFI_ERROR(gop->QueryMode(gop, mode, &isz, &mi)) || !mi) continue;
        puts_("  m"); dec_(mode); putc_(' '); dec_(mi->HorizontalResolution); putc_('x');
        dec_(mi->VerticalResolution); puts_(" f"); dec_(mi->PixelFormat);
        if (mode == m->Mode) puts_(" <-cur");
        nl_();
    }
}

static void paint_marker(void) {
    if (!g_gop || !g_gop->Mode || !g_gop->Mode->FrameBufferBase || !g_gop->Mode->Info) return;
    u32 w = g_gop->Mode->Info->HorizontalResolution, h = g_gop->Mode->Info->VerticalResolution;
    u32 pitch = g_gop->Mode->Info->PixelsPerScanLine;
    if (g_gop->Mode->Info->PixelFormat != 0) return;   /* only RGBx is guessed safely */
    volatile u32 *fb = (volatile u32 *)(UINTN)g_gop->Mode->FrameBufferBase;
    for (u32 y = h - 24; y < h; y++)
        for (u32 x = 0; x < 120; x++)
            fb[(u64)y * pitch + x] = 0x00ff0000u;
}

/* -------------------------------- main ----------------------------------- */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    (void)image_handle;
    con_out = st->ConOut;
    if (con_out && con_out->SetMode) con_out->SetMode(con_out, 0);
    if (con_out) con_out->OutputString(con_out, (CHAR16 *)L"\r\nHobbyOS probe: booting\r\n");

    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_STATUS s;

    puts_("=== HobbyOS probe (aarch64 UEFI) ===\n");
    puts_("[fw] "); dec_(st->FirmwareRevision); puts_(" vendor=");
    if (st->FirmwareVendor) for (CHAR16 *p = st->FirmwareVendor; *p; p++) putc_((char)(*p & 0x7f));
    nl_();

    dump_acpi(st);
    dump_gop(st);
    dump_memmap(st);
    dump_pci();
    dump_pci_detail();
    scan_virtio_mmio(st);

    /* BlockIo inventory before the filesystem test, so a read-only disk shows */
    {
        UINTN count = 0; EFI_HANDLE *h = 0;
        if (!EFI_ERROR(bs->LocateHandleBuffer(2, (void *)GUID_BLOCK_IO, 0, &count, &h))) {
            puts_("[blk] handles: "); dec_(count); nl_();
            for (UINTN i = 0; i < count && i < 8; i++) {
                EFI_BLOCK_IO_PROTOCOL *bio = 0;
                if (EFI_ERROR(bs->HandleProtocol(h[i], (void *)GUID_BLOCK_IO, (void **)&bio)) || !bio) continue;
                EFI_BLOCK_IO_MEDIA *m = bio->Media;
                if (!m) continue;
                puts_("  "); dec_(i);
                puts_(" bs="); dec_(m->BlockSize);
                puts_(" last="); dec_(m->LastBlock);
                puts_(" ro="); dec_(m->ReadOnly ? 1 : 0);
                puts_(" present="); dec_(m->MediaPresent ? 1 : 0);
                puts_(" removable="); dec_(m->RemovableMedia ? 1 : 0);
                nl_();
            }
        }
    }

    s = try_write_file(st, "[fs] wrote file");
    if (EFI_ERROR(s)) { puts_("[fs] file write failed, using raw sectors\n"); raw_dump(st); }
    else raw_dump(st);

    puts_("=== probe complete ===\n");
    out_flush_screen();
    paint_marker();

    for (volatile u64 spin = 0;; spin++)
        if ((spin & 0xffffff) == 0) bs->Stall(100000);
    return EFI_SUCCESS;
}
