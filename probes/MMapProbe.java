/*
 * probes/MMapProbe.java -- FileChannel.map / MappedByteBuffer on HobbyOS.
 *
 * What "works" has to mean, precisely:
 *   - a READ_ONLY mapping of an existing file returns the file's bytes
 *     (this is the path the JDK's own DSO/jimage use in reverse: the kernel
 *     fills VM_OBJ_FILE pages from FAT16 on the first touch);
 *   - a READ_WRITE mapping is writable, and the write is a property of the
 *     FILE, not of the mapping -- i.e. a *separate* open + read(2) sees it.
 *     That is the only check that distinguishes "shared page cache" from
 *     "wrote to disk";
 *   - force() means "and now it is durable": after force(), the bytes must
 *     survive an independent reader.
 *
 * Every check prints PASS/FAIL with the exact exception text, so a failure is
 * a bug report rather than a verdict.
 */
import java.io.*;
import java.nio.*;
import java.nio.channels.*;
import java.nio.file.*;
import java.util.*;

public class MMapProbe {
    static int pass = 0, fail = 0, n = 0;
    static final String DIR = "/JDKTMP";

    interface Body { void run() throws Exception; }

    static void ok(String name, boolean cond, String detail) {
        int i = ++n;
        if (cond) { pass++; System.out.println("PASS " + i + " " + name); }
        else { fail++; System.out.println("FAIL " + i + " " + name + ": " + detail); }
    }

    static void check(String name, Body b) {
        try { b.run(); ok(name, true, ""); }
        catch (Throwable t) {
            Throwable c = t;
            while (c instanceof RuntimeException && c.getCause() != null
                   && c.getClass() == RuntimeException.class) c = c.getCause();
            ok(name, false, c.getClass().getName() + ": " + c.getMessage());
        }
    }

    static Path path(String n) { return Paths.get(DIR + "/" + n); }

    /* Write n bytes of a deterministic pattern with plain channel writes. */
    static void seed(Path p, int len) throws IOException {
        byte[] b = new byte[len];
        for (int i = 0; i < len; i++) b[i] = (byte) ('A' + (i % 26));
        try (FileChannel ch = FileChannel.open(p, StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE)) {
            ByteBuffer bb = ByteBuffer.wrap(b);
            while (bb.hasRemaining()) ch.write(bb);
            ch.force(true);
        }
    }

    /* The disk truth: a brand-new open + positional read, no mapping involved. */
    static byte[] readBack(Path p, int len) throws IOException {
        byte[] b = new byte[len];
        try (FileChannel ch = FileChannel.open(p, StandardOpenOption.READ)) {
            ByteBuffer bb = ByteBuffer.wrap(b);
            int off = 0;
            while (bb.hasRemaining()) {
                int r = ch.read(bb, off);
                if (r < 0) break;
                off += r;
            }
        }
        return b;
    }

    static String hex(byte[] b, int off, int len) {
        StringBuilder sb = new StringBuilder();
        for (int i = off; i < off + len && i < b.length; i++)
            sb.append(String.format("%02x", b[i] & 0xff));
        return sb.toString();
    }

    public static void main(String[] a) throws Exception {
        System.out.println("MMAP-PROBE: begin (java " + System.getProperty("java.version")
                           + ")");
        new File(DIR).mkdirs();

        final Path f = path("mmap.dat");
        final int LEN = 64 * 1024;          /* exactly 16 pages: no tail page */
        seed(f, LEN);

        /* 1. READ_ONLY mapping returns the file's bytes. */
        check("map READ_ONLY sees file content", () -> {
            try (FileChannel ch = FileChannel.open(f, StandardOpenOption.READ)) {
                MappedByteBuffer m = ch.map(FileChannel.MapMode.READ_ONLY, 0, LEN);
                ok("map.size", m.capacity() == LEN, "capacity=" + m.capacity());
                boolean good = true; int badAt = -1;
                for (int i = 0; i < LEN; i++) {
                    if (m.get(i) != (byte) ('A' + (i % 26))) { good = false; badAt = i; break; }
                }
                ok("map READ_ONLY content", good,
                   "first mismatch at " + badAt + " got=0x"
                   + (badAt >= 0 ? String.format("%02x", m.get(badAt) & 0xff) : "-"));
                ok("map READ_ONLY isReadOnly", m.isReadOnly(), "isReadOnly=false");
                ok("map READ_ONLY isDirect", m.isDirect(), "isDirect=false");
            }
        });

        /* 2. READ_WRITE mapping: write, read back through the SAME mapping. */
        final byte[] mark = new byte[] { 'W', 'R', 'I', 'T', 'E', '!', 0x7f, (byte) 0x80 };
        check("map READ_WRITE put/get", () -> {
            try (FileChannel ch = FileChannel.open(f, StandardOpenOption.READ,
                    StandardOpenOption.WRITE)) {
                MappedByteBuffer m = ch.map(FileChannel.MapMode.READ_WRITE, 0, LEN);
                ok("map RW isReadOnly=false", !m.isReadOnly(), "isReadOnly=true");
                for (int i = 0; i < mark.length; i++) m.put(4096 + i, mark[i]);
                boolean good = true;
                for (int i = 0; i < mark.length; i++)
                    if (m.get(4096 + i) != mark[i]) good = false;
                ok("map RW read back through mapping", good,
                   "got=" + hex(new byte[] { m.get(4096), m.get(4097) }, 0, 2));
                /* bulk relative put through a duplicate view */
                ByteBuffer d = m.duplicate();
                d.position(8192);
                byte[] bulk = { 'B', 'U', 'L', 'K' };
                d.put(bulk);
                ok("map RW bulk put", m.get(8192) == 'B' && m.get(8193) == 'U',
                   "got=" + (char) m.get(8192) + (char) m.get(8193));
            }
        });

        /* 3. force(): after this the bytes must be durable, i.e. visible to an
         *    independent open+read.  This is where a page-cache-only mmap shows
         *    itself. */
        check("map RW force() then disk read", () -> {
            try (FileChannel ch = FileChannel.open(f, StandardOpenOption.READ,
                    StandardOpenOption.WRITE)) {
                MappedByteBuffer m = ch.map(FileChannel.MapMode.READ_WRITE, 0, LEN);
                m.put(12288, (byte) 'Z');
                m.force();
                byte[] at = new byte[8];
                try (FileChannel r = FileChannel.open(f, StandardOpenOption.READ)) {
                    r.read(ByteBuffer.wrap(at), 12288);
                }
                ok("force() write reaches disk", at[0] == 'Z',
                   "byte at 12288 on disk = 0x" + String.format("%02x", at[0] & 0xff)
                   + " (expected 0x5a 'Z'); mapping itself reads "
                   + (char) m.get(12288));
            }
        });

        /* 4. Is the write at least shared between two mappings of one file
         *    (kernel page cache), even though it never reached the disk? */
        check("two mappings of one file share the page cache", () -> {
            try (FileChannel ca = FileChannel.open(f, StandardOpenOption.READ,
                         StandardOpenOption.WRITE);
                 FileChannel cb = FileChannel.open(f, StandardOpenOption.READ,
                         StandardOpenOption.WRITE)) {
                MappedByteBuffer ma = ca.map(FileChannel.MapMode.READ_WRITE, 0, LEN);
                MappedByteBuffer mb = cb.map(FileChannel.MapMode.READ_WRITE, 0, LEN);
                ma.put(16384, (byte) 'Q');
                boolean seen = mb.get(16384) == 'Q';
                byte[] disk = new byte[1];
                try (FileChannel r = FileChannel.open(f, StandardOpenOption.READ)) {
                    r.read(ByteBuffer.wrap(disk), 16384);
                }
                ok("second mapping sees the write", seen,
                   "got=0x" + String.format("%02x", mb.get(16384) & 0xff));
                System.out.println("NOTE mapping2=" + (seen ? "sees" : "misses")
                                   + " disk=0x" + String.format("%02x", disk[0] & 0xff)
                                   + " at offset 16384");
            }
        });

        /* 5. position is not page aligned: the JDK aligns and slices. */
        check("map at unaligned position", () -> {
            try (FileChannel ch = FileChannel.open(f, StandardOpenOption.READ)) {
                MappedByteBuffer m = ch.map(FileChannel.MapMode.READ_ONLY, 100, 1000);
                ok("unaligned map capacity", m.capacity() == 1000,
                   "capacity=" + m.capacity());
                ok("unaligned map content",
                   m.get(0) == (byte) ('A' + (100 % 26)),
                   "first byte=" + (char) m.get(0) + " expected="
                   + (char) ('A' + (100 % 26)));
            }
        });

        /* 6. A mapping longer than the file: Linux accepts it, pages past EOF
         *    read as zero.  Reading only (a store there is SIGBUS on Linux). */
        check("map longer than file reads zeros past EOF", () -> {
            try (FileChannel ch = FileChannel.open(f, StandardOpenOption.READ)) {
                MappedByteBuffer m = ch.map(FileChannel.MapMode.READ_ONLY, 0, LEN + 8192);
                int z = 0;
                for (int i = LEN; i < LEN + 8192; i++) if (m.get(i) == 0) z++;
                ok("tail past EOF is zero", z == 8192, "nonzero bytes in tail=" + (8192 - z));
            }
        });

        /* 7. Mapping a zero-length file: no pages to back it. */
        check("map of an empty file is refused", () -> {
            Path e = path("mmap-empty.dat");
            Files.deleteIfExists(e);
            try (FileChannel ch = FileChannel.open(e, StandardOpenOption.CREATE,
                    StandardOpenOption.WRITE)) {
                MappedByteBuffer m = ch.map(FileChannel.MapMode.READ_WRITE, 0, 4096);
                if (m != null)
                    throw new IOException("map() of a 0-byte file RETURNED a buffer of "
                                          + m.capacity() + " bytes");
            }
        });

        /* 8. load() / isLoaded(): advisory, must not throw. */
        check("MappedByteBuffer.load", () -> {
            try (FileChannel ch = FileChannel.open(f, StandardOpenOption.READ)) {
                MappedByteBuffer m = ch.map(FileChannel.MapMode.READ_ONLY, 0, LEN);
                m.load();
                ok("load() returns", true, "");
                System.out.println("NOTE isLoaded=" + m.isLoaded());
            }
        });

        System.out.println("SUMMARY pass=" + pass + " fail=" + fail);
        System.out.println("MMAP-PROBE: DONE");
    }
}
