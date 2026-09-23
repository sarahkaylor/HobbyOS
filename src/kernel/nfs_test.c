#ifdef KERNEL_MODE_UNIT_TEST

/*
 * nfs_test.c - Kernel (EL1) unit tests for the NFS client protocol codecs
 * and the VFS routing layer.  Runs on ARM64 and x86-64 via run_unit_tests.
 *
 * These tests cover everything that does not need a live server: XDR
 * round-trips and bounds checks, RPC message construction and reply
 * parsing against crafted fixtures, the mount-table API's empty cases,
 * path canonicalisation, and the FAT-backed side of the VFS dispatch
 * (the disk image is present in unit-test mode).
 *
 * The live end-to-end NFS behavior is covered by src/user/nfs_test.c
 * (MODE=test) against a real server, and by src/host/nfs_proto_test.c on
 * the development host.
 */

#include "unit_test.h"
#include "nfs.h"
#include "vfs.h"
#include "fs.h"
#include "process.h"

extern struct process *current_process(void);

/* ---- fixture builders ------------------------------------------------ */

static void wr_fattr3(struct xdr_w *w, uint32_t type, uint64_t size) {
    xdr_w_u32(w, type);          /* ftype3 type */
    xdr_w_u32(w, 0644);          /* mode3 */
    xdr_w_u32(w, 1);             /* nlink */
    xdr_w_u32(w, 0);             /* uid */
    xdr_w_u32(w, 0);             /* gid */
    xdr_w_u64(w, size);          /* size3 */
    xdr_w_u64(w, size);          /* used */
    xdr_w_u32(w, 0);             /* rdev major */
    xdr_w_u32(w, 0);             /* rdev minor */
    xdr_w_u64(w, 0x1234);        /* fsid */
    xdr_w_u64(w, 0x5678);        /* fileid */
    xdr_w_u32(w, 1000); xdr_w_u32(w, 0);   /* atime (nfstime3: sec + nsec) */
    xdr_w_u32(w, 2000); xdr_w_u32(w, 0);   /* mtime */
    xdr_w_u32(w, 3000); xdr_w_u32(w, 0);   /* ctime */
}

static void wr_post_op_attr(struct xdr_w *w, int present, uint32_t type,
                            uint64_t size) {
    xdr_w_u32(w, present ? 1 : 0);
    if (present) wr_fattr3(w, type, size);
}

/* ---- XDR ------------------------------------------------------------- */

static void test_xdr_roundtrip(void) {
    uart_puts("  Running test_xdr_roundtrip...\n");
    tests_run++;

    uint8_t buf[64];
    struct xdr_w w;
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 0x01020304);
    xdr_w_u64(&w, 0x1122334455667788ULL);
    xdr_w_string(&w, "abc");                          /* 4 + 3 + 1 pad */
    xdr_w_opaque(&w, (const uint8_t *)"xy", 2);       /* 4 + 2 + 2 pad */
    ASSERT(w.err == 0);
    EXPECT_EQ(w.len, 4 + 8 + 8 + 8);

    struct xdr_r r;
    xdr_r_init(&r, buf, w.len);
    EXPECT_EQ((int)xdr_r_u32(&r), 0x01020304);
    ASSERT(xdr_r_u64(&r) == 0x1122334455667788ULL);
    char s[8];
    EXPECT_EQ(xdr_r_string(&r, s, sizeof s), 3);
    ASSERT(s[0] == 'a' && s[1] == 'b' && s[2] == 'c' && s[3] == '\0');
    uint8_t o[4];
    EXPECT_EQ(xdr_r_opaque(&r, o, sizeof o), 2);
    ASSERT(o[0] == 'x' && o[1] == 'y');
    EXPECT_EQ(xdr_r_remain(&r), 0);
    ASSERT(xdr_r_ok(&r));

    /* Big-endian on the wire. */
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[3], 0x04);
    EXPECT_EQ(buf[11], 0x88);
}

static void test_xdr_bounds(void) {
    uart_puts("  Running test_xdr_bounds...\n");
    tests_run++;

    /* Reader: truncated u32 leaves an error flag set and yields 0. */
    uint8_t two[2] = {0xAA, 0xBB};
    struct xdr_r r;
    xdr_r_init(&r, two, 2);
    EXPECT_EQ((int)xdr_r_u32(&r), 0);
    ASSERT(!xdr_r_ok(&r));

    /* Reader: opaque longer than the buffer. */
    uint8_t bad[8] = {0, 0, 0, 16, 1, 2, 3, 4};
    xdr_r_init(&r, bad, 8);
    uint8_t out[16];
    EXPECT_EQ(xdr_r_opaque(&r, out, sizeof out), -1);
    ASSERT(!xdr_r_ok(&r));

    /* Reader: string truncation respects the cap. */
    uint8_t str[8] = {0, 0, 0, 3, 'a', 'b', 'c', 0};
    xdr_r_init(&r, str, 8);
    char small[2];
    EXPECT_EQ(xdr_r_string(&r, small, sizeof small), 3);
    ASSERT(small[0] == 'a' && small[1] == '\0');

    /* Writer: a write that does not fit sets the error flag and the
     * remaining bytes are simply not stored (callers discard on err). */
    uint8_t tiny[6];
    struct xdr_w w;
    xdr_w_init(&w, tiny, sizeof tiny);
    xdr_w_u64(&w, 1);
    ASSERT(w.err == 1);
    EXPECT_EQ(w.len, 4);        /* first half fit; the rest was refused */
}

/* ---- RPC ------------------------------------------------------------- */

static void test_rpc_build_call(void) {
    uart_puts("  Running test_rpc_build_call...\n");
    tests_run++;

    uint8_t args[8];
    struct xdr_w aw;
    xdr_w_init(&aw, args, sizeof args);
    xdr_w_fh(&aw, (const uint8_t *)"\xAA\xBB\xCC\xDD", 4);   /* len + data */
    ASSERT(aw.err == 0);

    uint8_t msg[128];
    int n = rpc_build_call(msg, sizeof msg, 0xDEADBEEF, NFS_PROG, NFS_VERS,
                           NFS_PROC_GETATTR, args, aw.len);
    EXPECT_EQ(n, 76);                       /* 24 hdr + 8 + 28 + 8 + 8 */

    struct xdr_r r;
    xdr_r_init(&r, msg, n);
    EXPECT_EQ((int)xdr_r_u32(&r), 0xDEADBEEF);      /* xid */
    EXPECT_EQ((int)xdr_r_u32(&r), RPC_MSG_CALL);
    EXPECT_EQ((int)xdr_r_u32(&r), RPC_VERSION);
    EXPECT_EQ((int)xdr_r_u32(&r), NFS_PROG);
    EXPECT_EQ((int)xdr_r_u32(&r), NFS_VERS);
    EXPECT_EQ((int)xdr_r_u32(&r), NFS_PROC_GETATTR);
    EXPECT_EQ((int)xdr_r_u32(&r), AUTH_UNIX);
    EXPECT_EQ((int)xdr_r_u32(&r), 28);
    EXPECT_EQ((int)xdr_r_u32(&r), 0xDEADBEEF);      /* stamp == xid */
    char mach[8];
    EXPECT_EQ(xdr_r_string(&r, mach, sizeof mach), 7);
    ASSERT(mach[0] == 'h' && mach[6] == 's');
    EXPECT_EQ((int)xdr_r_u32(&r), 0);               /* uid */
    EXPECT_EQ((int)xdr_r_u32(&r), 0);               /* gid */
    EXPECT_EQ((int)xdr_r_u32(&r), 0);               /* aux gid count */
    EXPECT_EQ((int)xdr_r_u32(&r), AUTH_NULL);       /* verifier flavor */
    EXPECT_EQ((int)xdr_r_u32(&r), 0);               /* verifier length */
    EXPECT_EQ((int)xdr_r_u32(&r), 4);               /* fh length */
    EXPECT_EQ((int)xdr_r_u32(&r), 0xAABBCCDD);      /* fh bytes, big-endian */
    ASSERT(xdr_r_ok(&r));

    /* Too small a buffer is an error, not a partial message. */
    EXPECT_EQ(rpc_build_call(msg, 40, 1, NFS_PROG, NFS_VERS, 1, args, aw.len), -1);
}

static void test_rpc_parse_reply(void) {
    uart_puts("  Running test_rpc_parse_reply...\n");
    tests_run++;

    uint8_t msg[64];
    struct xdr_w w;
    int bo = 0, bl = 0;

    /* Accepted reply, verifier with 5 bytes of body (3 pad), one body word. */
    xdr_w_init(&w, msg, sizeof msg);
    xdr_w_u32(&w, 0x1234);          /* xid */
    xdr_w_u32(&w, RPC_MSG_REPLY);
    xdr_w_u32(&w, RPC_MSG_ACCEPTED);
    xdr_w_u32(&w, AUTH_UNIX);       /* verifier flavor */
    xdr_w_u32(&w, 5);               /* verifier length */
    xdr_w_bytes(&w, (const uint8_t *)"ABCDE", 5);
    xdr_w_u32(&w, RPC_SUCCESS);     /* accept stat */
    xdr_w_u32(&w, 0xCAFEBABE);      /* body */
    ASSERT(w.err == 0);
    EXPECT_EQ(rpc_parse_reply(msg, w.len, 0x1234, &bo, &bl), RPC_OK);
    EXPECT_EQ(bl, 4);
    ASSERT(bo == w.len - 4);
    EXPECT_EQ((int)((uint32_t)msg[bo] << 24 | msg[bo + 1] << 16 |
                    msg[bo + 2] << 8 | msg[bo + 3]), 0xCAFEBABE);

    /* Wrong xid: the caller keeps reading other datagrams. */
    EXPECT_EQ(rpc_parse_reply(msg, w.len, 0x9999, &bo, &bl), RPC_E_WRONG_XID);

    /* Denied. */
    xdr_w_init(&w, msg, sizeof msg);
    xdr_w_u32(&w, 7);
    xdr_w_u32(&w, RPC_MSG_REPLY);
    xdr_w_u32(&w, RPC_MSG_DENIED);
    xdr_w_u32(&w, 0);               /* reject stat */
    xdr_w_u32(&w, 1);               /* auth stat: AUTH_BADCRED */
    EXPECT_EQ(rpc_parse_reply(msg, w.len, 7, &bo, &bl), RPC_E_DENIED);

    /* Accepted but procedure unavailable. */
    xdr_w_init(&w, msg, sizeof msg);
    xdr_w_u32(&w, 8);
    xdr_w_u32(&w, RPC_MSG_REPLY);
    xdr_w_u32(&w, RPC_MSG_ACCEPTED);
    xdr_w_u32(&w, AUTH_NULL);
    xdr_w_u32(&w, 0);
    xdr_w_u32(&w, RPC_PROG_UNAVAIL);
    EXPECT_EQ(rpc_parse_reply(msg, w.len, 8, &bo, &bl), RPC_E_DENIED);

    /* Truncated. */
    EXPECT_EQ(rpc_parse_reply(msg, 6, 8, &bo, &bl), RPC_E_MALFORMED);

    /* Not a reply at all. */
    EXPECT_EQ(rpc_parse_reply((const uint8_t *)"\x00\x00\x00\x01\x00\x00\x00\x00",
                              8, 1, &bo, &bl), RPC_E_MALFORMED);
}

/* ---- NFS decoders ---------------------------------------------------- */

static void test_parse_attrs_and_handles(void) {
    uart_puts("  Running test_parse_attrs_and_handles...\n");
    tests_run++;

    uint8_t buf[256];
    struct xdr_w w;
    struct nfs_attr a;
    struct nfs_fh fh;

    /* fattr3 through post_op_attr. */
    xdr_w_init(&w, buf, sizeof buf);
    wr_post_op_attr(&w, 1, NF3REG, 4242);
    ASSERT(w.err == 0);
    struct xdr_r r;
    xdr_r_init(&r, buf, w.len);
    int present = -1;
    EXPECT_EQ(nfs_parse_post_op_attr(&r, &a, &present), 0);
    ASSERT(present == 1);
    EXPECT_EQ((int)a.type, NF3REG);
    ASSERT(a.size == 4242);
    ASSERT(xdr_r_ok(&r) && xdr_r_remain(&r) == 0);

    /* post_op_attr absent: nothing follows. */
    xdr_w_init(&w, buf, sizeof buf);
    wr_post_op_attr(&w, 0, 0, 0);
    xdr_w_u32(&w, 0xFEED);              /* must be untouched */
    xdr_r_init(&r, buf, w.len);
    EXPECT_EQ(nfs_parse_post_op_attr(&r, &a, &present), 0);
    ASSERT(present == 0);
    EXPECT_EQ((int)xdr_r_u32(&r), 0xFEED);

    /* post_op_fh3. */
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 1);
    xdr_w_fh(&w, (const uint8_t *)"\x01\x02\x03\x04\x05", 5);
    xdr_r_init(&r, buf, w.len);
    EXPECT_EQ(nfs_parse_post_op_fh(&r, &fh), 0);
    EXPECT_EQ((int)fh.len, 5);
    ASSERT(fh.data[0] == 1 && fh.data[4] == 5);

    /* Oversized handle is refused, not truncated. */
    uint8_t big[4 + NFS_FH_MAX + 1 + 4];
    xdr_w_init(&w, big, sizeof big);
    xdr_w_u32(&w, 1);
    xdr_w_u32(&w, NFS_FH_MAX + 1);
    xdr_r_init(&r, big, w.len);
    EXPECT_EQ(nfs_parse_post_op_fh(&r, &fh), -1);

    /* Truncated fattr3 (one byte short). */
    xdr_w_init(&w, buf, sizeof buf);
    wr_fattr3(&w, NF3DIR, 7);
    xdr_r_init(&r, buf, w.len - 1);
    EXPECT_EQ(nfs_parse_fattr3(&r, &a), -1);
}

static void test_parse_replies(void) {
    uart_puts("  Running test_parse_replies...\n");
    tests_run++;

    uint8_t buf[256];
    struct xdr_w w;

    /* GETATTR success and refusal: the attributes are a bare fattr3. */
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 0);
    wr_fattr3(&w, NF3DIR, 0);
    struct nfs_attr a;
    EXPECT_EQ(nfs_parse_getattr_reply(buf, w.len, &a), 0);
    EXPECT_EQ((int)a.type, NF3DIR);
    ASSERT(a.mtime == 2000);
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 2);                    /* NFS3ERR_NOENT */
    EXPECT_EQ(nfs_parse_getattr_reply(buf, w.len, &a), -2);

    /* LOOKUP: the object handle is an nfs_fh3 (no presence flag). */
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 0);
    xdr_w_fh(&w, (const uint8_t *)"\xDE\xAD\xBE\xEF", 4);
    wr_post_op_attr(&w, 1, NF3REG, 512);
    wr_post_op_attr(&w, 0, 0, 0);
    struct nfs_fh fh;
    int attr_ok = 0;
    EXPECT_EQ(nfs_parse_lookup_reply(buf, w.len, &fh, &a, &attr_ok), 0);
    EXPECT_EQ((int)fh.len, 4);
    ASSERT(attr_ok == 1 && a.size == 512);

    /* LOOKUP that returns no handle must fail. */
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 0);
    xdr_w_u32(&w, 0);                    /* empty handle */
    wr_post_op_attr(&w, 0, 0, 0);
    wr_post_op_attr(&w, 0, 0, 0);
    EXPECT_EQ(nfs_parse_lookup_reply(buf, w.len, &fh, &a, &attr_ok), -2);

    /* READ. */
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 0);
    wr_post_op_attr(&w, 0, 0, 0);
    xdr_w_u32(&w, 4);                    /* count */
    xdr_w_u32(&w, 1);                    /* eof */
    xdr_w_opaque(&w, (const uint8_t *)"DATA", 4);
    uint8_t out[8];
    uint32_t count = 0;
    int eof = 0;
    EXPECT_EQ(nfs_parse_read_reply(buf, w.len, out, sizeof out, &count, &eof), 0);
    EXPECT_EQ((int)count, 4);
    ASSERT(eof == 1);
    ASSERT(out[0] == 'D' && out[3] == 'A');

    /* READ with a count that disagrees with the data field is refused. */
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 0);
    wr_post_op_attr(&w, 0, 0, 0);
    xdr_w_u32(&w, 9);                    /* count */
    xdr_w_u32(&w, 0);
    xdr_w_opaque(&w, (const uint8_t *)"DATA", 4);
    EXPECT_EQ(nfs_parse_read_reply(buf, w.len, out, sizeof out, &count, &eof), -1);

    /* FSSTAT. */
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 0);
    wr_post_op_attr(&w, 0, 0, 0);
    xdr_w_u64(&w, 1000000);
    xdr_w_u64(&w, 400000);
    xdr_w_u64(&w, 300000);
    xdr_w_u64(&w, 100);
    xdr_w_u64(&w, 50);
    xdr_w_u64(&w, 40);
    xdr_w_u32(&w, 0);
    uint64_t total = 0, avail = 0;
    EXPECT_EQ(nfs_parse_fsstat_reply(buf, w.len, &total, &avail), 0);
    ASSERT(total == 1000000 && avail == 400000);

    /* MOUNT MNT: status 0, handle, auth flavors list. */
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 0);
    xdr_w_fh(&w, (const uint8_t *)"\x10\x20\x30\x40", 4);
    xdr_w_u32(&w, 2);
    xdr_w_u32(&w, AUTH_NULL);
    xdr_w_u32(&w, AUTH_UNIX);
    int auth_ok = 0;
    EXPECT_EQ(nfs_parse_mount_reply(buf, w.len, &fh, &auth_ok), 0);
    EXPECT_EQ((int)fh.len, 4);
    ASSERT(auth_ok == 1);

    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 0);
    xdr_w_fh(&w, (const uint8_t *)"\x10", 1);
    xdr_w_u32(&w, 1);
    xdr_w_u32(&w, AUTH_NULL);
    EXPECT_EQ(nfs_parse_mount_reply(buf, w.len, &fh, &auth_ok), 0);
    ASSERT(auth_ok == 0);

    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 13);                   /* MNT3ERR_ACCES */
    EXPECT_EQ(nfs_parse_mount_reply(buf, w.len, &fh, &auth_ok), -2);

    /* GETPORT. */
    xdr_w_init(&w, buf, sizeof buf);
    xdr_w_u32(&w, 2049);
    uint32_t port = 0;
    EXPECT_EQ(nfs_parse_getport_reply(buf, w.len, &port), 0);
    ASSERT(port == 2049);
    EXPECT_EQ(nfs_parse_getport_reply(buf, 2, &port), -1);
}

static void test_parse_readdirplus(void) {
    uart_puts("  Running test_parse_readdirplus...\n");
    tests_run++;

    uint8_t buf[512];
    struct xdr_w w;
    char name[NFS_NAME_MAX];
    struct nfs_attr a;
    int attr_ok = 0;
    uint64_t cookie = 0;

    xdr_w_init(&w, buf, sizeof buf);
    /* entry 1: dir */
    xdr_w_u32(&w, 1);
    xdr_w_u64(&w, 11);
    xdr_w_string(&w, "sub");
    xdr_w_u64(&w, 100);
    wr_post_op_attr(&w, 1, NF3DIR, 0);
    xdr_w_u32(&w, 0);                    /* no handle */
    /* entry 2: file */
    xdr_w_u32(&w, 1);
    xdr_w_u64(&w, 12);
    xdr_w_string(&w, "a.txt");
    xdr_w_u64(&w, 200);
    wr_post_op_attr(&w, 1, NF3REG, 55);
    xdr_w_u32(&w, 0);
    /* end of list */
    xdr_w_u32(&w, 0);
    ASSERT(w.err == 0);

    struct xdr_r r;
    xdr_r_init(&r, buf, w.len);
    EXPECT_EQ(nfs_readdirplus_entry(&r, name, sizeof name, &a, &attr_ok, &cookie), 1);
    ASSERT(name[0] == 's' && name[3] == '\0');
    ASSERT(cookie == 100 && a.type == NF3DIR);
    EXPECT_EQ(nfs_readdirplus_entry(&r, name, sizeof name, &a, &attr_ok, &cookie), 1);
    ASSERT(name[0] == 'a' && a.size == 55);
    ASSERT(cookie == 200);
    EXPECT_EQ(nfs_readdirplus_entry(&r, name, sizeof name, &a, &attr_ok, &cookie), 0);
    ASSERT(xdr_r_ok(&r) && xdr_r_remain(&r) == 0);

    /* A truncated entry must be an error, not a silent end of list. */
    xdr_r_init(&r, buf, 20);
    EXPECT_EQ(nfs_readdirplus_entry(&r, name, sizeof name, &a, &attr_ok, &cookie), -1);
    /* Only the "entry follows" flag present: also an error. */
    struct xdr_r r2;
    xdr_r_init(&r2, buf, 4);
    EXPECT_EQ(nfs_readdirplus_entry(&r2, name, sizeof name, &a, &attr_ok, &cookie), -1);
}

/* ---- VFS paths and routing ------------------------------------------- */

static void test_vfs_path_cleaning(void) {
    uart_puts("  Running test_vfs_path_cleaning...\n");
    tests_run++;

    char out[VFS_PATH_MAX];
    EXPECT_EQ(vfs_abs_path("/", out, sizeof out), 0);
    ASSERT(out[0] == '/' && out[1] == '\0');
    vfs_abs_path("/a/b/c", out, sizeof out);
    ASSERT(out[0] == '/' && out[1] == 'a' && out[2] == '/' && out[3] == 'b');
    vfs_abs_path("/a/b/../c", out, sizeof out);
    ASSERT(out[0] == '/' && out[1] == 'a' && out[2] == '/' && out[3] == 'c' &&
           out[4] == '\0');
    vfs_abs_path("//x//y///", out, sizeof out);
    ASSERT(out[0] == '/' && out[1] == 'x' && out[2] == '/' && out[3] == 'y' &&
           out[4] == '\0');
    vfs_abs_path("/a/./b/", out, sizeof out);
    ASSERT(out[1] == 'a' && out[2] == '/' && out[3] == 'b' && out[4] == '\0');
    vfs_abs_path("/../../x", out, sizeof out);
    ASSERT(out[1] == 'x' && out[2] == '\0');
    vfs_abs_path("/", out, 1);           /* cap 1: just NUL */
    ASSERT(out[0] == '\0');

    /* Relative paths join onto the process cwd. */
    struct process *cur = current_process();
    char saved[128];
    saved[0] = '\0';
    if (cur) {
        int k = 0;
        while (cur->cwd[k] && k < 127) { saved[k] = cur->cwd[k]; k++; }
        saved[k] = '\0';
        cur->cwd[0] = '/'; cur->cwd[1] = '\0';
    }
    vfs_abs_path("file.txt", out, sizeof out);
    ASSERT(out[0] == '/' && out[1] == 'f');
    vfs_abs_path("..", out, sizeof out);
    ASSERT(out[0] == '/' && out[1] == '\0');
    vfs_abs_path(".", out, sizeof out);
    ASSERT(out[0] == '/' && out[1] == '\0');
    if (cur) {
        int k = 0;
        while (saved[k] && k < 127) { cur->cwd[k] = saved[k]; k++; }
        cur->cwd[k] = '\0';
    }

    /* Routing with an empty mount table: nothing goes to NFS. */
    int idx = -1;
    const char *rel = 0;
    EXPECT_EQ(vfs_route("/", &idx, &rel), 0);
    EXPECT_EQ(vfs_route("/nfs/x", &idx, &rel), 0);
    EXPECT_EQ(nfs_route("relative", &idx, &rel), 0);
    EXPECT_EQ(nfs_mount_count(), 0);
    ASSERT(nfs_mount_at(0) == 0);
    EXPECT_EQ(nfs_mount_remove("/nfs"), -1);
    struct vfs_mountinfo mi;
    EXPECT_EQ(vfs_mount_info(0, &mi), -1);
}

static void test_vfs_mount_validation(void) {
    uart_puts("  Running test_vfs_mount_validation...\n");
    tests_run++;

    /* Source parsing: the stored u32 must have the address's octets in
     * memory order (what net_socket_connect() writes to the wire). Getting
     * this wrong silently addresses the wrong host. */
    uint32_t ip = 0;
    char export[NFS_EXPORT_MAX];
    ASSERT(nfs_parse_source("10.0.2.2:/srv/nfs/export", &ip, export,
                            sizeof export) == 0);
    const uint8_t *b = (const uint8_t *)&ip;
    ASSERT(b[0] == 10 && b[1] == 0 && b[2] == 2 && b[3] == 2);
    /* "/srv/nfs/export" is 15 chars: 'e','x','p','o','r','t' occupy 9..14 */
    ASSERT(export[0] == '/' && export[1] == 's' && export[14] == 't' &&
           export[15] == '\0');

    ASSERT(nfs_parse_source("192.168.0.105", &ip, export, sizeof export) == 0);
    b = (const uint8_t *)&ip;
    ASSERT(b[0] == 192 && b[1] == 168 && b[2] == 0 && b[3] == 105);
    ASSERT(export[0] == '/' && export[1] == '\0');   /* default export */

    ASSERT(nfs_parse_source("255.255.255.255:/x", &ip, export, sizeof export) == 0);
    b = (const uint8_t *)&ip;
    ASSERT(b[0] == 255 && b[3] == 255);

    /* Formatting round-trips. */
    char src[64];
    nfs_format_source(src, sizeof src, ip, "/x");     /* still 255.255.255.255 */
    ASSERT(src[0] == '2' && src[2] == '5' && src[3] == '.' &&
           src[15] == ':' && src[17] == 'x');
    nfs_parse_source("10.0.2.2", &ip, export, sizeof export);
    nfs_format_source(src, sizeof src, ip, "/");
    ASSERT(src[0] == '1' && src[1] == '0' && src[2] == '.' &&
           src[3] == '0' && src[5] == '2' && src[7] == '2' &&
           src[8] == ':');

    /* Rejects. */
    ASSERT(nfs_parse_source("not-an-ip:/x", &ip, export, sizeof export) == -1);
    ASSERT(nfs_parse_source("999.1.1.1:/x", &ip, export, sizeof export) == -1);
    ASSERT(nfs_parse_source("10.0.2:/x", &ip, export, sizeof export) == -1);
    ASSERT(nfs_parse_source("10.0.2.2:relative", &ip, export, sizeof export) == -1);

    /* Bare (non-network) validation failures: everything here is decided
     * before any RPC is attempted. */
    EXPECT_EQ(vfs_mount("zzz:/export", "/mnt"), -1);     /* bad source */
    EXPECT_EQ(vfs_mount("1.2.3.4:/x", "/"), -1);         /* over the root */
    EXPECT_EQ(vfs_umount("/nfs"), -1);                   /* nothing mounted */
    EXPECT_EQ(vfs_umount(0), -1);
    EXPECT_EQ(vfs_mount(0, "/mnt"), -1);
}

static void test_vfs_fat_dispatch(void) {
    uart_puts("  Running test_vfs_fat_dispatch...\n");
    tests_run++;

    /* With no NFS mounts, vfs_* must behave exactly like the FAT layer on
     * the real disk image. */
    char cwd[VFS_PATH_MAX];
    EXPECT_EQ(vfs_chdir("/", cwd, sizeof cwd), 0);
    ASSERT(cwd[0] == '/' && cwd[1] == '\0');
    EXPECT_EQ(vfs_chdir("/home", cwd, sizeof cwd), 0);
    ASSERT(cwd[0] == '/' && cwd[1] == 'h');
    EXPECT_EQ(vfs_chdir("/home/..", cwd, sizeof cwd), 0);
    ASSERT(cwd[0] == '/' && cwd[1] == '\0');
    EXPECT_EQ(vfs_chdir("/no-such-dir-xyz", cwd, sizeof cwd), -1);

    /* Root listing through the VFS: SH.BIN is always in the image. */
    char name[NFS_NAME_MAX];
    uint8_t attr = 0;
    uint32_t size = 0;
    int found_sh = 0;
    for (int i = 0; i < 64; i++) {
        if (vfs_read_dir("/", i, name, sizeof name, &attr, &size) != 0) break;
        const char want[] = "SH.BIN";
        int same = 1;
        for (int k = 0; k < 7; k++) {
            if (name[k] != want[k]) {
                same = 0;
                break;
            }
        }
        if (same && name[6] == '\0') found_sh = 1;
    }
    ASSERT(found_sh == 1);
    EXPECT_EQ(vfs_read_dir("/", 10000, name, sizeof name, &attr, &size), -1);

    /* stats and mutating ops route to FAT when no mount covers them. */
    uint64_t total = 0, freeb = 0;
    EXPECT_EQ(vfs_stats(&total, &freeb), 0);
    ASSERT(total > 0 && freeb > 0);
    EXPECT_EQ(vfs_unlink("/NO-SUCH-FILE.TXT"), -1);
    EXPECT_EQ(vfs_mkdir("/home"), -1);                   /* exists already */
    ASSERT(vfs_cwd_writable() == 1);

    /* Opening a plain path is not an NFS open. */
    struct file f;
    EXPECT_EQ(vfs_open_routed("/SH.BIN", &f), 0);

    /* A path that parses as NFS but has no mount behind it stays FAT. */
    EXPECT_EQ(vfs_open_routed("/nfs/anything.txt", &f), 0);
}

void nfs_test_suite(void) {
    uart_puts("nfs_test_suite:\n");
    test_xdr_roundtrip();
    test_xdr_bounds();
    test_rpc_build_call();
    test_rpc_parse_reply();
    test_parse_attrs_and_handles();
    test_parse_replies();
    test_parse_readdirplus();
    test_vfs_path_cleaning();
    test_vfs_mount_validation();
    test_vfs_fat_dispatch();
}

#endif // KERNEL_MODE_UNIT_TEST
