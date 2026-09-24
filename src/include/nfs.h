#ifndef NFS_H
#define NFS_H

#include <stdint.h>

/*
 * nfs.h - NFSv3 client (read-only, over UDP) + mount table for HobbyOS.
 *
 * Two layers:
 *   nfs_proto.c - pure XDR/RPC encoding and reply parsing. No I/O, no kernel
 *                 dependencies, so the same code is unit tested on the host
 *                 and in the kernel's EL1 suite.
 *   nfs.c       - the client proper: portmapper/MOUNT/NFS RPCs over UDP
 *                 sockets, the mount table, path resolution, and the
 *                 read-only operations vfs.c dispatches to.
 *
 * Scope: NFSv3 over UDP, read-only (GETATTR, LOOKUP, READ, READDIRPLUS,
 * FSSTAT, plus MOUNT MNT/UMNT and portmapper GETPORT).  There is no
 * WRITE/CREATE/REMOVE: NFS directories are presented read-only and the
 * kernel returns failure for mutations, exactly like a read-only mount.
 *
 * Every request and reply is kept under a 1500-byte Ethernet frame: the
 * network stack does not reassemble IP fragments, so the client must never
 * provoke one.  READ uses a 1024-byte rsize and READDIRPLUS a 1200-byte
 * maxcount hint.
 */

/* ---- sizes ----------------------------------------------------------- */
#define NFS_MAX_MOUNTS      4
#define NFS_PATH_MAX        128    /* mount point and cwd paths */
#define NFS_SRC_MAX         64     /* "a.b.c.d:/export" source string */
#define NFS_FH_MAX          64     /* NFSv3 file handles are <= 64 bytes */
#define NFS_NAME_MAX        32     /* matches struct sys_dirent.name */
#define NFS_MSG_MAX         1472   /* max UDP payload we ever send */
#define NFS_READ_CHUNK      1024   /* READ rsize */
#define NFS_READDIR_DCOUNT  512    /* READDIRPLUS dircount hint */
#define NFS_READDIR_MAXC    1200   /* READDIRPLUS maxcount hint */
#define NFS_DIR_CACHE_MAX   64     /* entries cached per directory listing */
#define NFS_RPC_TRIES       3
#define NFS_RPC_TIMEOUT_MS  500
#define NFS_EXPORT_MAX      96     /* exported directory path */

/* ---- RPC ------------------------------------------------------------- */
#define RPC_MSG_CALL        0
#define RPC_MSG_REPLY       1
#define RPC_VERSION         2
#define RPC_MSG_ACCEPTED    0
#define RPC_MSG_DENIED      1
#define RPC_SUCCESS         0
#define RPC_PROG_UNAVAIL    1
#define RPC_PROG_MISMATCH   2
#define RPC_PROC_UNAVAIL    3
#define RPC_GARBAGE_ARGS    4
#define AUTH_NULL           0
#define AUTH_UNIX           1

#define PORTMAP_PROG        100000
#define PORTMAP_VERS        2
#define PORTMAP_PROC_GETPORT 3

#define MOUNT_PROG          100005
#define MOUNT_VERS          3
#define MOUNT_PROC_MNT      1
#define MOUNT_PROC_UMNT     3

#define NFS_PROG            100003
#define NFS_VERS            3
#define NFS_PROC_NULL       0
#define NFS_PROC_GETATTR    1
#define NFS_PROC_LOOKUP     3
#define NFS_PROC_READ       6
#define NFS_PROC_READDIRPLUS 17
#define NFS_PROC_FSSTAT     18

/* ftype3 */
#define NF3REG              1
#define NF3DIR              2
#define NF3BLK              3
#define NF3CHR              4
#define NF3LNK              5
#define NF3SOCK             6
#define NF3FIFO             7

/* RPC call/reply parse results */
#define RPC_OK              0
#define RPC_E_MALFORMED     (-1)
#define RPC_E_WRONG_XID     (-2)   /* datagram from an older request: keep reading */
#define RPC_E_DENIED        (-3)   /* MSG_DENIED or accept_stat != SUCCESS */

struct nfs_attr {
  uint32_t type;      /* ftype3 */
  uint32_t mode;
  uint64_t size;
  uint32_t mtime;
};

struct nfs_fh {
  uint32_t len;
  uint8_t  data[NFS_FH_MAX];
};

struct nfs_mount {
  int      in_use;
  char     point[NFS_PATH_MAX];       /* mount point, e.g. "/nfs" */
  char     source[NFS_SRC_MAX];       /* canonical "ip:/export" */
  uint32_t server_ip;                 /* host byte order */
  uint16_t nfs_port;
  uint16_t mount_port;
  uint8_t  root_fh[NFS_FH_MAX];
  uint32_t root_fh_len;
  uint32_t read_size;
};

/* ---- XDR writer (nfs_proto.c) ---------------------------------------- */
struct xdr_w {
  uint8_t *buf;
  int cap;
  int len;
  int err;            /* 1 once a write did not fit */
};
void xdr_w_init(struct xdr_w *w, uint8_t *buf, int cap);
void xdr_w_u32(struct xdr_w *w, uint32_t v);
void xdr_w_u64(struct xdr_w *w, uint64_t v);
void xdr_w_bytes(struct xdr_w *w, const uint8_t *data, int len);   /* raw + pad */
void xdr_w_opaque(struct xdr_w *w, const uint8_t *data, int len);  /* len + raw + pad */
void xdr_w_string(struct xdr_w *w, const char *s);
void xdr_w_fh(struct xdr_w *w, const uint8_t *fh, uint32_t len);

/* ---- XDR reader (nfs_proto.c) ---------------------------------------- */
struct xdr_r {
  const uint8_t *buf;
  int len;
  int pos;
  int err;            /* 1 once a read ran past the end */
};
void xdr_r_init(struct xdr_r *r, const uint8_t *buf, int len);
uint32_t xdr_r_u32(struct xdr_r *r);
uint64_t xdr_r_u64(struct xdr_r *r);
int  xdr_r_opaque(struct xdr_r *r, uint8_t *out, int cap);  /* returns data length */
int  xdr_r_string(struct xdr_r *r, char *out, int cap);     /* returns length */
/* Fixed-length raw bytes (no length prefix, e.g. cookieverf3): returns 0. */
int  xdr_r_bytes(struct xdr_r *r, uint8_t *out, int n);
int  xdr_r_skip(struct xdr_r *r, int n);
int  xdr_r_ok(const struct xdr_r *r);
int  xdr_r_remain(const struct xdr_r *r);

/* ---- RPC message layer (nfs_proto.c) --------------------------------- */
/* Build a full RPC call (header + AUTH_UNIX cred + AUTH_NULL verf + args).
 * Returns the message length, or -1 when it does not fit. */
int rpc_build_call(uint8_t *buf, int cap, uint32_t xid, uint32_t prog,
                   uint32_t vers, uint32_t proc,
                   const uint8_t *args, int args_len);
/* Validate a reply: xid match, MSG_ACCEPTED, accept_stat SUCCESS.  On
 * RPC_OK, the body offset and length describe the procedure's reply body. */
int rpc_parse_reply(const uint8_t *buf, int len, uint32_t xid,
                    int *body_off, int *body_len);

/* ---- NFS reply parsers (nfs_proto.c) --------------------------------- */
int nfs_parse_fattr3(struct xdr_r *r, struct nfs_attr *a);
int nfs_parse_post_op_attr(struct xdr_r *r, struct nfs_attr *a, int *present);
int nfs_parse_post_op_fh(struct xdr_r *r, struct nfs_fh *fh);
/* nfs_fh3 (RFC 1813): length + bytes, no presence flag.  Returns -2 when the
 * server supplied an empty handle. */
int nfs_parse_fh3(struct xdr_r *r, struct nfs_fh *fh);

int nfs_parse_getattr_reply(const uint8_t *body, int len, struct nfs_attr *a);
int nfs_parse_lookup_reply(const uint8_t *body, int len, struct nfs_fh *fh,
                           struct nfs_attr *a, int *attr_ok);
int nfs_parse_read_reply(const uint8_t *body, int len, uint8_t *data,
                         int data_cap, uint32_t *count, int *eof);
int nfs_parse_fsstat_reply(const uint8_t *body, int len, uint64_t *total,
                           uint64_t *avail);
/* 0 ok; -2 when the server refused (status != 0); *auth_unix_ok reports
 * whether AUTH_UNIX is among the flavors the server offered. */
int nfs_parse_mount_reply(const uint8_t *body, int len, struct nfs_fh *fh,
                          int *auth_unix_ok);
int nfs_parse_getport_reply(const uint8_t *body, int len, uint32_t *port);
/* One dirlistplus3 node: 1 = entry parsed, 0 = end of list, -1 = malformed.
 * *cookie_out receives the entry's cookie (needed to continue the listing). */
int nfs_readdirplus_entry(struct xdr_r *r, char *name, int name_cap,
                          struct nfs_attr *a, int *attr_ok, uint64_t *cookie_out);

/* ---- client (nfs.c) -------------------------------------------------- */
void nfs_init(void);

/* "a.b.c.d[:/export]" parsing/formatting.  parse returns 0 and stores the
 * address in the byte order net_socket_connect() expects (in-memory bytes
 * equal to a.b.c.d); export defaults to "/" when the source names none.
 * Exposed (and unit tested) because getting this order wrong silently
 * addresses the wrong host. */
int  nfs_parse_source(const char *source, uint32_t *ip_out, char *export,
                      int export_cap);
void nfs_format_source(char *out, int cap, uint32_t ip, const char *export);

int  nfs_mount_count(void);
/* Borrow the mount at `idx` (0-based, in_use entries only): the pointer is
 * valid until the next mount/unmount.  0 when there is no such mount. */
const struct nfs_mount *nfs_mount_at(int idx);
/* Snapshot of one mount for the sysinfo(8) syscall.  type: 1 = NFS. */
int  nfs_mount_info(int idx, char *point, int pcap, char *source, int scap,
                    int *type);
int  nfs_mount_add(const char *source, const char *point);   /* 0 / -1 */
int  nfs_mount_remove(const char *point);                    /* 0 / -1 */
/* Route an absolute path: 1 when it lives under a mount (fills *idx and
 * *rel pointing INTO the mount, "/" for the mount root itself). */
int  nfs_route(const char *abs_path, int *idx, const char **rel);

int  nfs_lookup(const struct nfs_mount *m, const char *rel,
                struct nfs_fh *fh, struct nfs_attr *a);
int  nfs_getattr(const struct nfs_mount *m, const struct nfs_fh *fh,
                 struct nfs_attr *a);
int  nfs_read_file(const struct nfs_mount *m, const struct nfs_fh *fh,
                   uint32_t offset, void *buf, uint32_t len);
int  nfs_list_dir(const struct nfs_mount *m, const char *rel, int index,
                  char *name, int name_cap, uint8_t *attr_out, uint32_t *size_out);
int  nfs_statfs(const struct nfs_mount *m, const char *rel,
                uint64_t *total, uint64_t *avail);
int  nfs_is_dir(const struct nfs_mount *m, const char *rel);

#endif /* NFS_H */
