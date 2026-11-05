/*
 * rwnfs_probe.c — M5 (assets) gate probe: the real RimWorld tree served
 * read-only over NFSv3/UDP from the host (QEMU slirp: 10.0.2.2).
 *
 * What it proves, in order, on a real HobbyOS guest:
 *   1. FAT16 open-by-long-name (VFAT LFN) for a mixed-case 8.3-incompatible
 *      name staged with mcopy — settles frozen interface F3's "8.3 alias
 *      map vs LFN" question with evidence from the real driver.
 *   2. mount("10.0.2.2:/srv/nfs/rimworld", "/NFS") through the portmapper
 *      and MOUNT v3, and the kernel mount table (sysinfo(8)).
 *   3. READDIRPLUS listing of /NFS with the REAL long names
 *      (RimWorldLinux_Data, start_RimWorld.sh, UnityPlayer.so) and sizes.
 *   4. A byte-exact 64 KiB read of /NFS/UnityPlayer.so with a checksum the
 *      host recomputes from the same file (FNV-1a 32 + sum32).
 *   5. A >64-entry directory (/RimWorldLinux_Data/Managed: 102 entries),
 *      which is where the per-directory listing cache bound shows up.
 *   6. spawn() of a native ELF whose path lives on NFS — the exec-from-NFS
 *      question.  Before the VFS exec path exists this prints the kernel's
 *      "Failed to open file" line and reports GAP; with it, the child runs.
 *
 * Output protocol: one "[RW] ..." line per fact, "[RW] done failures=<n>"
 * last.  Built by tools/rw/run-rwnfs-probe.sh (no Makefile edit: the
 * Makefile is L0-owned).
 */
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include "libc.h"

#ifndef RW_NFS_SERVER
#define RW_NFS_SERVER "10.0.2.2"     /* QEMU slirp host alias */
#endif
#ifndef RW_NFS_ALT
#define RW_NFS_ALT    "192.168.0.123" /* host LAN address (fallback) */
#endif
#define RW_NFS_EXPORT "/srv/nfs/rimworld"
#define RW_NFS_MNT    "/NFS"

#define RW_LFN_NAME    "/RimWorldLinux_Data"   /* mixed case, 12.3-incompatible */
#define RW_LFN_UPPER   "/RIMWORLDLINUX_DATA"   /* case-insensitive retry        */
#define RW_LFN_BODY    "LFN-OK:RimWorldLinux_Data:0001\n"

static int checks = 0;
static int failures = 0;

static void note(const char *s) { print("[RW] "); print(s); print("\n"); }

static void check(int ok, const char *what) {
  checks++;
  if (!ok) failures++;
  print("[RW] ");
  print(ok ? "PASS " : "FAIL ");
  print(what);
  print("\n");
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) {
  int i = 0;
  for (;;) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; i++; }
}

/* ---- network readiness ------------------------------------------------ */

static int wait_for_ip(void) {
  struct sys_netinfo ni;
  for (int i = 0; i < 80; i++) {
    if (sysinfo(4, &ni, sizeof ni) == 0 && ni.ip != 0) {
      print("[RW] net ip=");
      print_dec((long)ni.ip);
      print("\n");
      return 0;
    }
    usleep(100000);
  }
  return -1;
}

/* ---- hex printing (libc's print_hex pads nothing useful here) ---------- */

static void put_hex32(uint32_t v) {
  static const char d[] = "0123456789abcdef";
  char out[9];
  for (int i = 0; i < 8; i++) out[i] = d[(v >> (28 - 4 * i)) & 0xF];
  out[8] = '\0';
  print(out);
}

static void put_hex8(uint8_t v) {
  static const char d[] = "0123456789abcdef";
  char out[3];
  out[0] = d[(v >> 4) & 0xF];
  out[1] = d[v & 0xF];
  out[2] = '\0';
  print(out);
}

/* ---- 1. FAT16 long-name open ----------------------------------------- */

static void lfn_test(void) {
  char buf[64];
  int fd = open(RW_LFN_NAME, O_RDONLY);
  if (fd < 0) {
    check(0, "FAT16 open by long mixed-case name (open failed)");
    return;
  }
  int n = (int)read(fd, buf, sizeof buf - 1);
  close(fd);
  int want = slen(RW_LFN_BODY);
  int ok = (n == want);
  for (int i = 0; ok && i < n; i++) if (buf[i] != RW_LFN_BODY[i]) ok = 0;
  check(ok, "FAT16 open+read by long mixed-case name '/RimWorldLinux_Data'");

  /* The all-caps spelling must reach the same entry (the FAT/LFN compare is
   * case-insensitive) — this is what a game's own path lookup relies on. */
  fd = open(RW_LFN_UPPER, O_RDONLY);
  check(fd >= 0, "FAT16 open by upper-cased long name '/RIMWORLDLINUX_DATA'");
  if (fd >= 0) close(fd);

  /* And read_dir must show the long name, not an 8.3 alias, so `ls` is
   * trustworthy for the staging tree. */
  int seen = 0;
  for (int i = 0; i < 512; i++) {
    struct sys_dirent ent;
    if (read_dir("/", i, &ent) != 0) break;
    if (seq((const char *)ent.name, "RimWorldLinux_Data")) { seen = 1; break; }
  }
  check(seen, "FAT16 read_dir(\"/\") shows the long name RimWorldLinux_Data");
}

/* ---- 3. NFS directory listing ---------------------------------------- */

static int list_count(const char *path, int max) {
  int i = 0;
  for (; i < max; i++) {
    struct sys_dirent ent;
    if (read_dir(path, i, &ent) != 0) break;
  }
  return i;
}

static int list_entry(const char *path, const char *name, uint32_t *size,
                      uint8_t *attr) {
  for (int i = 0; i < 512; i++) {
    struct sys_dirent ent;
    if (read_dir(path, i, &ent) != 0) break;
    if (seq((const char *)ent.name, name)) {
      if (size) *size = ent.size;
      if (attr) *attr = ent.attr;
      return 1;
    }
  }
  return 0;
}

static void list_print(const char *path, int max_print) {
  print("[RW] LIST ");
  print(path);
  print("\n");
  for (int i = 0; i < 512; i++) {
    struct sys_dirent ent;
    if (read_dir(path, i, &ent) != 0) break;
    if (i < max_print) {
      print("[RW]   ");
      print(ent.name);
      print(" size=");
      print_dec((long)ent.size);
      print((ent.attr & 0x10) ? " dir\n" : "\n");
    }
  }
}

/* ---- 4. 64 KiB checksummed read -------------------------------------- */

static void checksum_64k(const char *path, uint32_t expect_size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    check(0, "NFS open UnityPlayer.so");
    return;
  }
  long size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  print("[RW] SIZE ");
  print(path);
  print(" bytes=");
  print_dec(size);
  print("\n");
  if (expect_size) check((uint32_t)size == expect_size, "UnityPlayer.so size matches the host");

  static uint8_t buf[4096];
  uint32_t h = 2166136261u;      /* FNV-1a 32 */
  uint32_t sum = 0;
  uint32_t got = 0;
  uint8_t first[8], last[8];
  int first_n = 0;
  while (got < 65536) {
    int want = (int)(65536 - got);
    if (want > (int)sizeof buf) want = (int)sizeof buf;
    int r = (int)read(fd, buf, want);
    if (r <= 0) break;
    for (int i = 0; i < r; i++) {
      uint8_t b = buf[i];
      h = (h ^ b) * 16777619u;
      sum += b;
      if (first_n < 8) first[first_n++] = b;
      last[(got + (uint32_t)i) & 7] = b;
    }
    got += (uint32_t)r;
  }
  close(fd);
  print("[RW] SUM ");
  print(path);
  print(" bytes=");
  print_dec((long)got);
  print(" fnv1a32=0x");
  put_hex32(h);
  print(" sum32=0x");
  put_hex32(sum);
  print(" first8=");
  for (int i = 0; i < 8; i++) put_hex8(first[i]);
  print(" last8=");
  /* last[0..7] holds the final 8 bytes but rotated by got&7; rotate back. */
  for (int i = 0; i < 8; i++) put_hex8(last[(got + (uint32_t)i) & 7]);
  print("\n");
  check(got == 65536, "64 KiB read of UnityPlayer.so completed");
}

/* ---- 5b. one read() must return the whole count ----------------------- *
 * A regular-file read(2) does not stop at the transport's rsize.  The NFS
 * client used to issue one READ RPC per call and return 1024 bytes for any
 * larger request; the loop above hides that (it just iterates), but Unity
 * reads ScriptingAssemblies.json (2915 bytes) with a SINGLE read() and gives
 * up when the count is short -- which empties the assembly list and kills the
 * player on mono_class_get_image(NULL).  This check is the regression: it
 * fails before the fix (got=1024) and passes after (got=2915). */
static void oneshot_nfs(void) {
  const char *path = RW_NFS_MNT "/RimWorldLinux_Data/ScriptingAssemblies.json";
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    check(0, "open ScriptingAssemblies.json over NFS");
    return;
  }
  static uint8_t b[4096];
  int n = (int)read(fd, b, 2915);
  close(fd);
  print("[RW] ONESHOT want=2915 got=");
  print_dec(n);
  print("\n");
  check(n == 2915, "one read() of a 2915-byte NFS file returns 2915 bytes");
}

/* ---- 6. exec from NFS ------------------------------------------------- */

static void exec_from_nfs(void) {
  /* The fixture is the tree's own native dynamic pilot (HDYN.BIN: ET_DYN,
   * PT_INTERP=/LDHOBBY.SO, DT_NEEDED LIBC.SO) copied onto the export.  It is
   * the smallest real ELF whose load path is the native one; it prints
   * "HDYN: done" to the console and returns 0, which the gate script greps
   * for.  (A static ET_EXEC cannot serve here: with no PT_INTERP the loader
   * puts it on the Linux-ABI compat track by design — M0 freeze.) */
  const char *path = RW_NFS_MNT "/hdyn.bin";
  char *argv[2];
  argv[0] = (char *)path;
  argv[1] = 0;
  int pid = spawn_ex(path, argv, 0, 0, 0);
  print("[RW] EXEC spawn_ex(\"");
  print(path);
  print("\") -> ");
  print_dec(pid);
  print("\n");
  if (pid < 0) {
    note("EXECFROMNFS GAP: the v2 ELF loader never opened the NFS path (see "
         "the kernel's \"Failed to open file\" line above)");
    check(0, "exec a native ELF from NFS");
    return;
  }
  int status = -1;
  int wp = waitpid(pid, &status, 0);
  print("[RW] EXEC waitpid=");
  print_dec(wp);
  print(" status=");
  print_dec(status);
  print("\n");
  check(wp == pid && status == 0, "exec a native ELF from NFS (child exit 0)");
}

/* ---- 3c. a path component longer than the 31-char dirent cap ---------- */

static void long_component_lookup(void) {
  /* 63 characters — RimWorld's longest file name.  readdir can only display
   * 31 (struct sys_dirent.name[32]); LOOKUP must not inherit that cap. */
  const char *p = RW_NFS_MNT "/RimWorldLinux_Data/Managed/"
                  "UnityEngine.RuntimeInitializeOnLoadManagerInitializerModule.dll";
  int fd = open(p, O_RDONLY);
  long sz = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
  if (fd >= 0) close(fd);
  print("[RW] LONGNAME size=");
  print_dec(sz);
  print(" path=");
  print(p);
  print("\n");
  check(sz == 9728, "open a 63-char path component over NFS (LOOKUP cap)");
}

/* ---- 3b. mmap of an NFS file (the DSO page-in question) --------------- */

static void mmap_from_nfs(void) {
  int fd = open(RW_NFS_MNT "/UnityPlayer.so", O_RDONLY);
  if (fd < 0) {
    check(0, "open UnityPlayer.so for mmap");
    return;
  }
  errno = 0;
  void *p = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
  print("[RW] MMAP ");
  print(RW_NFS_MNT "/UnityPlayer.so");
  print(" -> ");
  print_dec((long)p);
  print(" errno=");
  print_dec(errno);
  print("\n");
  check(p != MAP_FAILED, "mmap a page of an NFS file (DSO page-in path)");
  if (p != MAP_FAILED) munmap(p, 4096);
  close(fd);
}

/* ---------------------------------------------------------------------- */

__attribute__((section(".text._start"))) void _start(void) {
  note("rwnfs_probe start");

  /* 1. FAT16 LFN — before the mount shadows anything. */
  lfn_test();

  /* 2. Network + mount. */
  if (wait_for_ip() != 0) {
    check(0, "network came up (DHCP)");
    print("[RW] done failures=");
    print_dec(failures);
    print("\n");
    exit(failures);
  }
  check(1, "network came up (DHCP)");

  const char *source = RW_NFS_SERVER ":" RW_NFS_EXPORT;
  int rc = mount(source, RW_NFS_MNT);
  if (rc != 0) {
    note("mount via the slirp alias failed; trying the host LAN address");
    source = RW_NFS_ALT ":" RW_NFS_EXPORT;
    rc = mount(source, RW_NFS_MNT);
  }
  print("[RW] MOUNT source=");
  print(source);
  print(" target=");
  print(RW_NFS_MNT);
  print(" rc=");
  print_dec(rc);
  print("\n");
  check(rc == 0, "mount the RimWorld export");
  if (rc != 0) {
    print("[RW] done failures=");
    print_dec(failures);
    print("\n");
    exit(failures);
  }

  /* Mount table. */
  struct sys_mountinfo mi[4];
  int nm = sysinfo(8, mi, sizeof mi);
  print("[RW] MOUNTS n=");
  print_dec(nm);
  print("\n");
  for (int i = 0; i < nm && i < 4; i++) {
    print("[RW]   point=");
    print(mi[i].point);
    print(" source=");
    print(mi[i].source);
    print(" type=");
    print_dec(mi[i].type);
    print("\n");
  }
  check(nm >= 1 && seq(mi[0].point, RW_NFS_MNT) && seq(mi[0].source, source) &&
        mi[0].type == 1, "sysinfo(8) mount table shows /NFS as an NFS mount");

  /* 3. Listing. */
  list_print(RW_NFS_MNT, 32);
  uint32_t sz = 0;
  uint8_t at = 0;
  check(list_entry(RW_NFS_MNT, "RimWorldLinux_Data", &sz, &at) && (at & 0x10),
        "listing shows RimWorldLinux_Data (directory)");
  check(list_entry(RW_NFS_MNT, "start_RimWorld.sh", &sz, 0),
        "listing shows start_RimWorld.sh");
  check(list_entry(RW_NFS_MNT, "UnityPlayer.so", &sz, 0) && sz == 33010672u,
        "listing shows UnityPlayer.so with the host size 33010672");

  /* 4. 64 KiB checksum. */
  checksum_64k(RW_NFS_MNT "/UnityPlayer.so", 33010672u);
  oneshot_nfs();

  /* 5. A >64-entry directory: the listing cache bound is visible here. */
  int managed = list_count(RW_NFS_MNT "/RimWorldLinux_Data/Managed", 512);
  print("[RW] DIRCOUNT /NFS/RimWorldLinux_Data/Managed n=");
  print_dec(managed);
  print(" expected=102\n");
  check(managed == 102, "Managed lists all 102 entries (listing cache bound)");

  /* A nested 15 MB read must open and report the right size. */
  int fd = open(RW_NFS_MNT "/RimWorldLinux_Data/Managed/Assembly-CSharp.dll", O_RDONLY);
  long vsz = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
  if (fd >= 0) close(fd);
  print("[RW] NESTED Assembly-CSharp.dll size=");
  print_dec(vsz);
  print("\n");
  check(vsz == 15767552, "nested open of Managed/Assembly-CSharp.dll");

  /* 6. long component, mmap of an NFS file (DSO page-in), exec from NFS. */
  long_component_lookup();
  mmap_from_nfs();
  exec_from_nfs();

  print("[RW] done failures=");
  print_dec(failures);
  print("\n");
  exit(failures);
}
