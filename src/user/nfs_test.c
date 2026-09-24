/*
 * nfs_test.c - Integration test for the HobbyOS NFS client and the mount
 * syscalls (SYS_MOUNT=27 / SYS_UMOUNT=28), run from the in-OS test mode
 * (MODE=test) on both ARM64 and x86-64.
 *
 * Tier 1 (always runs, no server required)
 *   - mount() source parsing and target validation
 *   - unreachable-server behavior: failure is fast and leaves no mount entry
 *   - umount() of a path with no mount
 *   - the FAT volume keeps working around it
 *
 * Tier 2 (requires an NFS server; skipped with a loud SKIP line otherwise)
 *   - mount the host export, check the sysinfo(8) mount table
 *   - directory listings (READDIRPLUS), including attributes and sizes
 *   - byte-exact file reads: small files, a 10000-byte chunked pattern file,
 *     nested directories, relative paths
 *   - FSSTAT through sysinfo(7), read-only enforcement for mutations,
 *     duplicate/nested mount refusal, unmount and post-unmount behavior
 *
 * The server address defaults to 10.0.2.2 (the host under QEMU slirp
 * user-networking) and can be overridden with -DNFS_TEST_SERVER="\"a.b.c.d\"".
 */

#include "libc.h"

#ifndef NFS_TEST_SERVER
#define NFS_TEST_SERVER "10.0.2.2"
#endif

#define NFS_TEST_EXPORT   "/srv/nfs/export"
#define NFS_TEST_SOURCE   NFS_TEST_SERVER ":" NFS_TEST_EXPORT
#define NFS_TEST_MOUNT    "/nfs"
#define NFS_TEST_BOGUS_IP "192.0.2.1"      /* TEST-NET-1: never routable */
#define NFS_TEST_ALT_IP   "192.168.0.105"  /* host LAN address fallback */

static int checks = 0;
static int failed = 0;

/* ---- tiny helpers (no libc string functions in the kernel image) ---- */

static int slen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static int seq(const char *a, const char *b) {
  int i = 0;
  for (;;) {
    if (a[i] != b[i]) return 0;
    if (!a[i]) return 1;
    i++;
  }
}

static void check(int ok, const char *name) {
  checks++;
  if (ok) {
    print("[NFS] PASS ");
  } else {
    failed++;
    print("[NFS] FAIL ");
  }
  print(name);
  print("\n");
}

static void note(const char *s) {
  print("[NFS] ");
  print(s);
  print("\n");
}

/* ---- directory listing collector ------------------------------------ */

#define MAX_LIST 96
static char l_name[MAX_LIST][32];
static uint8_t l_attr[MAX_LIST];
static uint32_t l_size[MAX_LIST];
static int l_count;

static int name_is_dot(const char *n) {
  if (n[0] != '.') return 0;
  if (n[1] == '\0') return 1;
  return n[1] == '.' && n[2] == '\0';
}

/* Collect the children of `path`, skipping the "." / ".." entries FAT
 * stores inside subdirectories (the listing consumers synthesize their own
 * parent row). */
static int list_dir(const char *path) {
  l_count = 0;
  for (int i = 0; i < MAX_LIST; i++) {
    struct sys_dirent ent;
    if (read_dir(path, i, &ent) != 0) break;
    if (name_is_dot(ent.name)) continue;
    int k = 0;
    while (ent.name[k] && k < 31) {
      l_name[l_count][k] = ent.name[k];
      k++;
    }
    l_name[l_count][k] = '\0';
    l_attr[l_count] = ent.attr;
    l_size[l_count] = ent.size;
    l_count++;
  }
  return l_count;
}

static int list_index(const char *name) {
  for (int i = 0; i < l_count; i++)
    if (seq(l_name[i], name)) return i;
  return -1;
}

/* ---- network readiness ---------------------------------------------- */

static int wait_for_ip(void) {
  struct sys_netinfo ni;
  for (int i = 0; i < 80; i++) {
    if (sysinfo(4, &ni, sizeof ni) == 0 && ni.ip != 0) return 0;
    usleep(100000);
  }
  return -1;
}

/* ---- file read helpers ---------------------------------------------- */

static char rbuf[1400];

/* Read a whole file, comparing each byte against expectation `fill`:
 * 0 = expect the literal `expect` string, 1 = expect the (i*7+3)%256
 * pattern used by PATTERN.BIN. Returns bytes read, or -1. */
static int read_expect(const char *path, const char *expect, int pattern,
                       int max_len) {
  int fd = open(path, 0);
  if (fd < 0) return -1;
  int total = 0;
  for (;;) {
    int r = read(fd, rbuf, sizeof rbuf);
    if (r < 0) {
      close(fd);
      return -1;
    }
    if (r == 0) break;
    for (int i = 0; i < r; i++) {
      unsigned want = pattern
          ? (unsigned)(((total + i) * 7 + 3) & 0xFF)
          : (unsigned char)expect[total + i];
      if ((unsigned char)rbuf[i] != want) {
        close(fd);
        return -1;
      }
    }
    total += r;
    if (total > max_len) {
      close(fd);
      return -1;
    }
  }
  close(fd);
  return total;
}

__attribute__((section(".text._start"))) void _start(void) {
  note("nfs_test starting");

  if (wait_for_ip() != 0) {
    check(0, "network came up (DHCP)");
    print("[NFS] FAILED (no IP, aborting)\n");
    exit(1);
  }
  check(1, "network came up (DHCP)");

  struct sys_mountinfo mi[4];

  /* ================= Tier 1: no server required ================= */

  check(mount(NFS_TEST_BOGUS_IP ":/x", NFS_TEST_MOUNT) != 0,
        "tier1: unreachable server fails the mount");

  /* The failed mount must not have registered anything. */
  check(sysinfo(8, mi, sizeof mi) == 0, "tier1: no mounts after failure");

  check(mount("not-an-ip:/x", "/mnt") != 0, "tier1: bad source refused");
  check(mount("999.1.1.1:/x", "/mnt") != 0, "tier1: out-of-range IP refused");
  check(mount(NFS_TEST_SOURCE, "/") != 0, "tier1: mounting over / refused");
  check(umount(NFS_TEST_MOUNT) != 0, "tier1: umount with no mount refused");
  check(umount("/nothing-here") != 0, "tier1: umount of unknown path refused");

  /* Before mounting, /nfs is a plain (empty) FAT directory. */
  check(list_dir(NFS_TEST_MOUNT) == 0,
        "tier1: /nfs is an empty FAT dir before mounting");
  check(chdir(NFS_TEST_MOUNT "/does-not-exist") != 0,
        "tier1: chdir into a missing dir fails");

  /* ================= Tier 2: real server ======================== */

  /* Try the slirp host address first, then the host's LAN address: some
   * QEMU builds only forward guest->host UDP for addresses that are real
   * host interfaces, not for the 10.0.2.2 alias. */
  const char *used_source = NFS_TEST_SOURCE;
  int mounted = 0;
  if (mount(NFS_TEST_SOURCE, NFS_TEST_MOUNT) == 0) {
    mounted = 1;
  } else if (mount(NFS_TEST_ALT_IP ":" NFS_TEST_EXPORT, NFS_TEST_MOUNT) == 0) {
    mounted = 1;
    used_source = NFS_TEST_ALT_IP ":" NFS_TEST_EXPORT;
    note("mounted via the alternate host address (slirp alias had no UDP path)");
  }
  if (!mounted) {
    note("SKIP e2e: no NFS server reachable at " NFS_TEST_SERVER
         " or " NFS_TEST_ALT_IP " - tier 2 not exercised");
  } else {
    mounted = 1;
    check(1, "mount " NFS_TEST_SOURCE " at " NFS_TEST_MOUNT);

    /* Mount table via sysinfo(8). */
    int n = sysinfo(8, mi, sizeof mi);
    check(n == 1, "sysinfo(8) reports one mount");
    check(seq(mi[0].point, NFS_TEST_MOUNT), "mount table point matches");
    check(seq(mi[0].source, used_source), "mount table source matches");
    check(mi[0].type == 1, "mount table type is NFS");

    /* Directory listing. */
    int count = list_dir(NFS_TEST_MOUNT);
    check(count == 6, "listing has 6 entries");
    int i_hello = list_index("HELLO.TXT");
    int i_e2e = list_index("E2E.TXT");
    int i_empty = list_index("EMPTY.TXT");
    int i_patt = list_index("PATTERN.BIN");
    int i_sub = list_index("SUBDIR");
    int i_bulk = list_index("BULK");
    check(i_hello >= 0 && i_e2e >= 0 && i_empty >= 0 && i_patt >= 0 &&
          i_sub >= 0 && i_bulk >= 0,
          "listing has the expected names");
    if (i_hello >= 0 && i_e2e >= 0 && i_empty >= 0 && i_patt >= 0 &&
        i_sub >= 0 && i_bulk >= 0) {
      check(l_attr[i_sub] == 0x10, "SUBDIR is reported as a directory");
      check(l_attr[i_bulk] == 0x10, "BULK is reported as a directory");
      check(l_attr[i_hello] == 0, "HELLO.TXT is reported as a file");
      check(l_size[i_hello] == 22, "HELLO.TXT size is 22");
      check(l_size[i_e2e] == 63, "E2E.TXT size is 63");
      check(l_size[i_empty] == 0, "EMPTY.TXT size is 0");
      check(l_size[i_patt] == 10000, "PATTERN.BIN size is 10000");
    }
    check(read_dir(NFS_TEST_MOUNT, 6, &(struct sys_dirent){0}) != 0,
          "listing past the last entry fails");
    check(list_dir(NFS_TEST_MOUNT "/SUBDIR") == 1 &&
          seq(l_name[0], "NESTED.TXT"),
          "nested listing shows NESTED.TXT");

    /* A 20-entry directory needs several READDIRPLUS batches: this
     * exercises the cookie/verifier continuation path. */
    {
      int nb = list_dir(NFS_TEST_MOUNT "/BULK");
      check(nb == 20, "BULK listing returns all 20 entries");
      int i7 = list_index("BULK07.TXT");
      check(i7 >= 0 && l_size[i7] == 8, "BULK07.TXT is 8 bytes");
      int seen = 1;
      for (int i = 0; i < 20; i++) {
        char want[16];
        /* BULK00.TXT .. BULK19.TXT */
        int k = 0;
        const char *p = "BULK";
        while (p[k]) { want[k] = p[k]; k++; }
        want[k++] = (char)('0' + (i / 10));
        want[k++] = (char)('0' + (i % 10));
        int k2 = 0;
        const char *q = ".TXT";
        while (q[k2]) { want[k + k2] = q[k2]; k2++; }
        want[k + k2] = '\0';
        if (list_index(want) < 0) seen = 0;
      }
      check(seen == 1, "every BULKnn.TXT entry is present");
      check(read_expect(NFS_TEST_MOUNT "/BULK/BULK07.TXT", "bulk 07\n", 0, 32) == 8,
            "read BULK/BULK07.TXT byte-exact");
    }

    /* Byte-exact reads. */
    check(read_expect(NFS_TEST_MOUNT "/HELLO.TXT",
                      "HELLO FROM NFS SERVER\n", 0, 64) == 22,
          "read HELLO.TXT byte-exact (22 bytes)");
    check(read_expect(NFS_TEST_MOUNT "/E2E.TXT",
                      "Line one of E2E file.\nLine two has words.\n"
                      "Line three, the end.\n", 0, 128) == 63,
          "read E2E.TXT byte-exact (63 bytes)");
    check(read_expect(NFS_TEST_MOUNT "/EMPTY.TXT", "", 0, 4) == 0,
          "read EMPTY.TXT yields 0 bytes");

    /* Multi-chunk read: 10000 bytes at 1024 per RPC. */
    {
      int fd = open(NFS_TEST_MOUNT "/PATTERN.BIN", 0);
      int ok = 1;
      int total = 0;
      int first = 1;
      if (fd < 0) {
        ok = 0;
      } else {
        for (;;) {
          int r = read(fd, rbuf, sizeof rbuf);   /* 1400 > 1024 */
          if (r < 0) { ok = 0; break; }
          if (r == 0) break;
          if (first && r != 1024) ok = 0;   /* clamped to rsize */
          first = 0;
          for (int i = 0; i < r; i++) {
            if ((unsigned char)rbuf[i] !=
                (unsigned char)(((total + i) * 7 + 3) & 0xFF)) {
              ok = 0;
              break;
            }
          }
          if (!ok) break;
          total += r;
        }
        close(fd);
      }
      check(ok && total == 10000,
            "read PATTERN.BIN (10000 bytes, chunked 1024/RPC)");
    }

    /* Nested + relative paths. */
    check(read_expect(NFS_TEST_MOUNT "/SUBDIR/NESTED.TXT",
                      "NESTED CONTENT OK\n", 0, 64) == 18,
          "read SUBDIR/NESTED.TXT byte-exact");
    check(chdir(NFS_TEST_MOUNT) == 0, "chdir into the mount");
    char cwdbuf[128];
    check(getcwd(cwdbuf, sizeof cwdbuf) != 0 && seq(cwdbuf, NFS_TEST_MOUNT),
          "getcwd reports the mount path");
    check(read_expect("HELLO.TXT", "HELLO FROM NFS SERVER\n", 0, 64) == 22,
          "relative path read from inside the mount");
    check(chdir("SUBDIR") == 0 &&
          read_expect("NESTED.TXT", "NESTED CONTENT OK\n", 0, 64) == 18,
          "relative path read from a nested mount dir");
    check(getcwd(cwdbuf, sizeof cwdbuf) != 0 &&
          seq(cwdbuf, NFS_TEST_MOUNT "/SUBDIR"),
          "getcwd tracks the nested dir");
    check(chdir("..") == 0 && getcwd(cwdbuf, sizeof cwdbuf) != 0 &&
          seq(cwdbuf, NFS_TEST_MOUNT),
          "chdir .. returns to the mount root");

    /* FSSTAT through sysinfo(7). */
    {
      struct sys_fsinfo fi;
      check(sysinfo(7, &fi, sizeof fi) == 0 && fi.total_bytes > 0 &&
            fi.free_bytes > 0,
            "sysinfo(7) reports NFS FSSTAT numbers");
    }

    /* Read-only enforcement. */
    check(unlink(NFS_TEST_MOUNT "/HELLO.TXT") != 0,
          "unlink on the mount is refused");
    check(rename(NFS_TEST_MOUNT "/E2E.TXT", NFS_TEST_MOUNT "/X.TXT") != 0,
          "rename on the mount is refused");
    check(mkdir(NFS_TEST_MOUNT "/NEWDIR") != 0,
          "mkdir on the mount is refused");
    check(open(NFS_TEST_MOUNT "/NO-SUCH-FILE.TXT", 0) < 0,
          "open of a missing NFS file fails");

    /* Duplicate / nested mounts. */
    check(mount(NFS_TEST_SOURCE, NFS_TEST_MOUNT) != 0,
          "duplicate mount point refused");
    check(mount(NFS_TEST_SOURCE, NFS_TEST_MOUNT "/sub") != 0,
          "mount inside a mount refused");

    /* A mount point that does not exist yet is created on demand. */
    check(mount(NFS_TEST_SOURCE, "/NFS2") == 0, "mount creates a missing point");
    check(list_dir("/") >= 0 && list_index("NFS2") >= 0,
          "the created mount point shows up in the FAT root listing");
    check(umount("/NFS2") == 0, "umount of the second mount");

    /* Reads through a handle opened before the unmount must fail
     * cleanly, not crash, once the mount is gone. */
    {
      int fd = open(NFS_TEST_MOUNT "/HELLO.TXT", 0);
      int r1 = fd >= 0 ? read(fd, rbuf, 5) : -1;
      check(r1 == 5, "short read before unmount");
      check(umount(NFS_TEST_MOUNT) == 0, "umount the mount");
      int r2 = read(fd, rbuf, 5);
      check(r2 < 0, "read after unmount fails cleanly");
      close(fd);
    }
    check(sysinfo(8, mi, sizeof mi) == 0, "mount table empty after unmount");
    check(getcwd(cwdbuf, sizeof cwdbuf) != 0 && seq(cwdbuf, "/"),
          "cwd fell back to / after its mount went away");
    {
      int left = list_dir(NFS_TEST_MOUNT);
      if (left != 0) {
        note("post-unmount /nfs still lists entries; count/first:");
        print_dec(left);
        print(" ");
        note(left > 0 ? l_name[0] : "(none)");
      }
      check(left == 0, "the mount point is an empty FAT dir again");
    }
  }

  /* ================= FAT still works ============================ */

  check(chdir("/") == 0, "chdir /");
  if (list_dir("/") > 0 && list_index("SH.BIN") >= 0) {
    int fd = open("/SH.BIN", 0);
    int r = fd >= 0 ? read(fd, rbuf, 64) : -1;
    if (fd >= 0) close(fd);
    check(r == 64, "FAT file reads still work after NFS activity");
  } else {
    check(0, "FAT root listing shows SH.BIN");
  }
  {
    struct sys_fsinfo fi;
    check(sysinfo(7, &fi, sizeof fi) == 0 && fi.total_bytes > 0,
          "sysinfo(7) reports FAT numbers at /");
  }

  note(mounted ? "e2e tier exercised" : "e2e tier skipped");
  if (failed == 0) {
    print("[NFS] ALL PASSED (");
    print_dec(checks);
    print(" checks)\n");
    exit(0);
  }
  print("[NFS] FAILED (");
  print_dec(failed);
  print(" of ");
  print_dec(checks);
  print(")\n");
  exit(1);
}
