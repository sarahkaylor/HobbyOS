#!/usr/bin/env python3
"""Capture real RPC/NFSv3 replies from the local Linux nfsd.

Produces src/host/nfs_fixtures.h: byte arrays of datagrams exactly as the
server sent them (request encoding mirrors src/kernel/nfs_proto.c), so the
HobbyOS parsers are tested against real kernel-nfsd output rather than
hand-rolled fixtures.

Run: python3 tools/capture_nfs_fixtures.py
"""

import socket
import struct
import sys

HOST = "127.0.0.1"
EXPORT = "/srv/nfs/export"
HELLO = "/srv/nfs/export/HELLO.TXT"
SUBDIR = "/srv/nfs/export/SUBDIR"

PORTMAP_PROG, PORTMAP_VERS, PORTMAP_GETPORT = 100000, 2, 3
MOUNT_PROG, MOUNT_VERS, MOUNT_MNT = 100005, 3, 1
NFS_PROG, NFS_VERS = 100003, 3
NFS_GETATTR, NFS_LOOKUP, NFS_READ, NFS_READDIRPLUS, NFS_FSSTAT = 1, 3, 6, 17, 18

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(3.0)


def u32(v):
    return struct.pack(">I", v)


def u64(v):
    return struct.pack(">Q", v)


def xdr_string(s):
    b = s.encode()
    pad = (4 - len(b) % 4) % 4
    return u32(len(b)) + b + b"\0" * pad


def xdr_opaque(b):
    pad = (4 - len(b) % 4) % 4
    return u32(len(b)) + b + b"\0" * pad


def rpc_call(port, prog, vers, proc, args, xid):
    cred = u32(xid) + xdr_string("hobbyos") + u32(0) + u32(0) + u32(0)
    msg = (u32(xid) + u32(0) + u32(2) + u32(prog) + u32(vers) + u32(proc) +
           u32(1) + u32(len(cred)) + cred + u32(0) + u32(0) + args)
    sock.sendto(msg, (HOST, port))
    data, _ = sock.recvfrom(8192)
    # sanity: reply to our xid
    assert struct.unpack(">I", data[:4])[0] == xid, "xid mismatch"
    return data


def rpc_call_record(port, prog, vers, proc, args, xid, req_name, reply_name,
                    comment):
    """Like rpc_call but records both the request and the reply datagram."""
    cred = u32(xid) + xdr_string("hobbyos") + u32(0) + u32(0) + u32(0)
    msg = (u32(xid) + u32(0) + u32(2) + u32(prog) + u32(vers) + u32(proc) +
           u32(1) + u32(len(cred)) + cred + u32(0) + u32(0) + args)
    sock.sendto(msg, (HOST, port))
    data, _ = sock.recvfrom(8192)
    assert struct.unpack(">I", data[:4])[0] == xid, "xid mismatch"
    FIXTURES.append((req_name, comment + " [request]", msg, xid))
    FIXTURES.append((reply_name, comment + " [reply]", data, xid))
    return data


def getport(prog, vers):
    r = rpc_call(111, PORTMAP_PROG, PORTMAP_VERS, PORTMAP_GETPORT,
                 struct.pack(">IIII", prog, vers, 17, 0), 0x11000001)
    return struct.unpack(">I", r[-4:])[0]


def body_of(reply):
    """Slice the accepted-reply body out of a datagram (see rpc_parse_reply)."""
    pos = 4 * 3                      # xid, REPLY, ACCEPTED
    verf_flavor, verf_len = struct.unpack(">II", reply[pos:pos + 8])
    pos += 8 + verf_len + (4 - verf_len % 4) % 4
    accept = struct.unpack(">I", reply[pos:pos + 4])[0]
    assert accept == 0, "rpc denied"
    return reply, pos + 4            # full datagram, body offset


FIXTURES = []                        # (name, comment, datagram bytes, xid)


def capture(name, comment, reply, xid):
    FIXTURES.append((name, comment, reply, xid))


# The GETPORT request/reply pair is also kept as a golden pair: the host
# tests re-encode the request with rpc_build_call() and compare byte for
# byte, and parse the reply with the C decoders.
rpc_call_record(111, PORTMAP_PROG, PORTMAP_VERS, PORTMAP_GETPORT,
                struct.pack(">IIII", MOUNT_PROG, MOUNT_VERS, 17, 0), 0x11000001,
                "FIX_REQ_GETPORT", "FIX_REPLY_GETPORT",
                "portmapper GETPORT for MOUNT v3 udp")


# --- MOUNT MNT -------------------------------------------------------------
mount_port = getport(MOUNT_PROG, MOUNT_VERS)
nfs_port = getport(NFS_PROG, NFS_VERS)
print(f"mountd udp port {mount_port}, nfsd udp port {nfs_port}")

reply = rpc_call(mount_port, MOUNT_PROG, MOUNT_VERS, MOUNT_MNT,
                 xdr_string(EXPORT), 0x11000002)
capture("FIX_MNT", "MOUNT MNT /srv/nfs/export (v3, udp)", reply, 0x11000002)
# --- small XDR reader for the capture script -------------------------------

class Rd:
    def __init__(self, buf, pos):
        self.b = buf
        self.p = pos

    def u32(self):
        v = struct.unpack(">I", self.b[self.p:self.p + 4])[0]
        self.p += 4
        return v

    def u64(self):
        v = struct.unpack(">Q", self.b[self.p:self.p + 8])[0]
        self.p += 8
        return v

    def opaque(self):
        n = self.u32()
        d = self.b[self.p:self.p + n]
        self.p += n + (4 - n % 4) % 4
        return d

    def fattr3(self):
        t = self.u32(); mode = self.u32(); self.u32(); self.u32(); self.u32()
        size = self.u64(); self.u64(); self.u32(); self.u32()
        self.u64(); self.u64()
        for _ in range(3):             # nfstime3 = uint32 seconds + uint32 nsecs
            self.u32(); self.u32()
        return t, size

    def post_op_attr(self):
        if self.u32():
            return self.fattr3()
        return None


_, off = body_of(reply)
rd = Rd(reply, off)
status = rd.u32()
assert status == 0, f"MNT status {status}"
root_fh = rd.opaque()
print(f"root fh len {len(root_fh)}")

# --- NFS GETATTR on the root ----------------------------------------------
reply = rpc_call(nfs_port, NFS_PROG, NFS_VERS, NFS_GETATTR,
                 xdr_opaque(root_fh), 0x11000003)
capture("FIX_GETATTR_ROOT", "NFS GETATTR on the export root (a directory)",
        reply, 0x11000003)
_, off = body_of(reply)
rd = Rd(reply, off)
assert rd.u32() == 0
root_attrs = rd.fattr3()            # GETATTR3resok: bare fattr3
assert root_attrs[0] == 2, "root should be NF3DIR"

# --- LOOKUP HELLO.TXT ------------------------------------------------------
lookup_args = xdr_opaque(root_fh) + xdr_string("HELLO.TXT")
reply = rpc_call(nfs_port, NFS_PROG, NFS_VERS, NFS_LOOKUP, lookup_args, 0x11000004)
capture("FIX_LOOKUP_HELLO", "NFS LOOKUP HELLO.TXT in the root", reply, 0x11000004)
_, off = body_of(reply)
rd = Rd(reply, off)
assert rd.u32() == 0
hello_fh = rd.opaque()             # LOOKUP3resok.object: plain nfs_fh3
attrs = rd.post_op_attr()
assert attrs is not None
print(f"HELLO.TXT fh len {len(hello_fh)} type {attrs[0]} size {attrs[1]}")
assert attrs[0] == 1 and attrs[1] == 22, "HELLO.TXT should be a 22-byte file"

# --- READ HELLO.TXT --------------------------------------------------------
read_args = xdr_opaque(hello_fh) + u64(0) + u32(1024)
reply = rpc_call(nfs_port, NFS_PROG, NFS_VERS, NFS_READ, read_args, 0x11000005)
capture("FIX_READ_HELLO", "NFS READ 1024 bytes at offset 0 of HELLO.TXT",
        reply, 0x11000005)
_, off = body_of(reply)
rd = Rd(reply, off)
assert rd.u32() == 0
rd.post_op_attr()
count = rd.u32()
eof = rd.u32()
data = rd.opaque()
print(f"READ count {count} eof {eof} data {data!r}")
assert count == len(data) == 22
assert data == b"HELLO FROM NFS SERVER\n"

# --- READDIRPLUS of the root ----------------------------------------------
rd_args = xdr_opaque(root_fh) + u64(0) + b"\0" * 8 + u32(512) + u32(1200)
reply = rpc_call(nfs_port, NFS_PROG, NFS_VERS, NFS_READDIRPLUS, rd_args, 0x11000006)
capture("FIX_READDIRPLUS_ROOT", "NFS READDIRPLUS of the export root",
        reply, 0x11000006)

# A deliberately small READDIRPLUS: the server truncates the listing and the
# trailing eof word must come back 0 (more entries remain).  The client has
# to continue with the last cookie.  maxcount must still fit one or two
# entries (an entry with attributes and a handle is ~160 bytes).
rd_args = xdr_opaque(root_fh) + u64(0) + b"\0" * 8 + u32(128) + u32(400)
reply = rpc_call(nfs_port, NFS_PROG, NFS_VERS, NFS_READDIRPLUS, rd_args, 0x11000008)
capture("FIX_READDIRPLUS_PARTIAL",
        "NFS READDIRPLUS of the root, truncated (eof word 0)", reply, 0x11000008)

# --- FSSTAT of the root ----------------------------------------------------
reply = rpc_call(nfs_port, NFS_PROG, NFS_VERS, NFS_FSSTAT,
                 xdr_opaque(root_fh), 0x11000007)
capture("FIX_FSSTAT_ROOT", "NFS FSSTAT of the export root", reply, 0x11000007)

# --- write the header ------------------------------------------------------
lines = []
lines.append("/* Generated by tools/capture_nfs_fixtures.py - DO NOT EDIT.")
lines.append(" *")
lines.append(" * Real RPC/NFSv3 reply datagrams captured from the Linux nfsd on")
lines.append(" * this machine.  The host unit tests parse these with the same")
lines.append(" * codecs the kernel uses, so the fixtures pin parser behavior to")
lines.append(" * actual server output.  File handles differ per server run; the")
lines.append(" * tests only parse the replies, never replay the handles.")
lines.append(" */")
lines.append("#ifndef NFS_FIXTURES_H")
lines.append("#define NFS_FIXTURES_H")
lines.append("")

expected_names = None
for name, comment, data, xid in FIXTURES:
    lines.append(f"/* {comment} */")
    lines.append(f"#define {name}_XID 0x{xid:08X}u")
    lines.append(f"#define {name}_LEN {len(data)}")
    body = ", ".join(f"0x{b:02X}" for b in data)
    lines.append(f"static const unsigned char {name}[{len(data)}] = {{ {body} }};")
    lines.append("")

lines.append("#endif /* NFS_FIXTURES_H */")

with open("src/host/nfs_fixtures.h", "w") as f:
    f.write("\n".join(lines) + "\n")

print(f"wrote src/host/nfs_fixtures.h with {len(FIXTURES)} fixtures")
for name, comment, data, xid in FIXTURES:
    print(f"  {name}: {len(data)} bytes")
