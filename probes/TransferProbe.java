/*
 * probes/TransferProbe.java -- the non-mapped file-channel surface:
 * transferTo/transferFrom, truncate/extend, and positional (pread/pwrite) I/O.
 *
 * On Linux transferTo(FileChannel) is sendfile(2); JDK 27 falls back to a
 * MAPPED transfer when sendfile answers ENOSYS/EINVAL, and to a read/write
 * loop when the mapping is refused too (FileChannelImpl.transferToDirect ->
 * transferToTrustedChannel(16 KiB threshold) -> transferToArbitraryChannel).
 * So a transfer check that only compares bytes cannot say which path ran; the
 * NOTE lines record the sizes and timings, and the kernel log records the
 * sendfile/mmio attempts.  The byte comparison is what is being *claimed*.
 */
import java.io.*;
import java.net.*;
import java.nio.*;
import java.nio.channels.*;
import java.nio.file.*;

public class TransferProbe {
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

    static byte[] pattern(int len) {
        byte[] b = new byte[len];
        for (int i = 0; i < len; i++) b[i] = (byte) ('a' + (i % 26));
        return b;
    }

    static void seed(Path p, int len) throws IOException {
        try (FileChannel ch = FileChannel.open(p, StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE)) {
            ByteBuffer bb = ByteBuffer.wrap(pattern(len));
            while (bb.hasRemaining()) ch.write(bb);
        }
    }

    static byte[] slurp(Path p) throws IOException {
        long sz = Files.size(p);
        byte[] b = new byte[(int) sz];
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

    static boolean same(byte[] x, byte[] y, int xoff, int yoff, int len) {
        if (len < 0 || xoff + len > x.length || yoff + len > y.length) return false;
        for (int i = 0; i < len; i++) if (x[xoff + i] != y[yoff + i]) return false;
        return true;
    }

    public static void main(String[] a) throws Exception {
        System.out.println("TRANSFER-PROBE: begin (java " + System.getProperty("java.version") + ")");
        new File(DIR).mkdirs();

        final int BIG = 300 * 1024;
        final Path src = path("t-src.dat");
        seed(src, BIG);
        final byte[] srcBytes = pattern(BIG);

        /* 1. transferTo file->file, above the 16 KiB mapped threshold. */
        check("transferTo file->file 300KiB", () -> {
            Path dst = path("t-dst1.dat");
            Files.deleteIfExists(dst);
            try (FileChannel in = FileChannel.open(src, StandardOpenOption.READ);
                 FileChannel out = FileChannel.open(dst, StandardOpenOption.CREATE,
                         StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE)) {
                long t0 = System.nanoTime();
                long n = in.transferTo(0, BIG, out);
                long ms = (System.nanoTime() - t0) / 1_000_000;
                System.out.println("NOTE transferTo big -> " + n + " bytes in " + ms + "ms");
                ok("transferTo big count", n == BIG, "returned " + n + " expected " + BIG);
                out.force(true);
            }
            byte[] got = slurp(dst);
            ok("transferTo big content", got.length == BIG && same(got, srcBytes, 0, 0, BIG),
               "dst length=" + got.length + " first mismatch="
               + firstDiff(got, srcBytes, BIG));
        });

        /* 2. transferTo below the mapped threshold (read/write loop path). */
        check("transferTo file->file 4KiB @1000", () -> {
            Path dst = path("t-dst2.dat");
            Files.deleteIfExists(dst);
            try (FileChannel in = FileChannel.open(src, StandardOpenOption.READ);
                 FileChannel out = FileChannel.open(dst, StandardOpenOption.CREATE,
                         StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE)) {
                long n = in.transferTo(1000, 4096, out);
                System.out.println("NOTE transferTo small -> " + n + " bytes");
                ok("transferTo small count", n == 4096, "returned " + n);
            }
            byte[] got = slurp(dst);
            ok("transferTo small content", same(got, srcBytes, 0, 1000, 4096),
               "first mismatch=" + firstDiff(got, srcBytes, 4096));
        });

        /* 3. transferFrom. */
        check("transferFrom file->file 64KiB @4096", () -> {
            Path dst = path("t-dst3.dat");
            Files.deleteIfExists(dst);
            try (FileChannel in = FileChannel.open(src, StandardOpenOption.READ);
                 FileChannel out = FileChannel.open(dst, StandardOpenOption.CREATE,
                         StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE)) {
                long n = out.transferFrom(in, 4096, 65536);
                System.out.println("NOTE transferFrom -> " + n + " bytes");
                ok("transferFrom count", n == 65536, "returned " + n);
            }
            byte[] got = slurp(dst);
            ok("transferFrom content", got.length >= 4096 + 65536
               && same(got, srcBytes, 4096, 0, 65536),
               "dst length=" + got.length + " first mismatch="
               + firstDiff(java.util.Arrays.copyOfRange(got, 4096,
                     Math.min(got.length, 4096 + 65536)), srcBytes, 65536));
        });

        /* 4. transferTo clamps at EOF. */
        check("transferTo clamps at EOF", () -> {
            Path dst = path("t-dst4.dat");
            Files.deleteIfExists(dst);
            try (FileChannel in = FileChannel.open(src, StandardOpenOption.READ);
                 FileChannel out = FileChannel.open(dst, StandardOpenOption.CREATE,
                         StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE)) {
                long n = in.transferTo(BIG - 1000, 50000, out);
                ok("transferTo past EOF count", n == 1000, "returned " + n + " expected 1000");
            }
        });

        /* 5. transferTo a socket (sendfile's real target). */
        check("transferTo file->socket", () -> {
            try (ServerSocketChannel srv = ServerSocketChannel.open()) {
                srv.bind(new InetSocketAddress(InetAddress.getLoopbackAddress(), 0));
                int port = ((InetSocketAddress) srv.getLocalAddress()).getPort();
                final long[] recvd = { -1 };
                Thread rx = new Thread(() -> {
                    try (SocketChannel sc = srv.accept()) {
                        ByteBuffer bb = ByteBuffer.allocate(65536);
                        long total = 0;
                        int r;
                        while ((r = sc.read(bb)) > 0) { total += r; bb.clear(); }
                        recvd[0] = total;
                    } catch (Throwable t) { System.out.println("NOTE rx: " + t); }
                });
                rx.start();
                try (SocketChannel out = SocketChannel.open(
                         new InetSocketAddress(InetAddress.getLoopbackAddress(), port));
                     FileChannel in = FileChannel.open(src, StandardOpenOption.READ)) {
                    long n = in.transferTo(0, 65536, out);
                    System.out.println("NOTE transferTo socket -> " + n);
                    ok("transferTo socket count", n == 65536, "returned " + n);
                    out.shutdownOutput();
                }
                rx.join(20000);
                ok("transferTo socket received", recvd[0] == 65536, "peer read " + recvd[0]);
            }
        });

        /* ---------------- truncate / extend ---------------- */
        check("truncate shrinks", () -> {
            Path t = path("t-trunc.dat");
            seed(t, 100 * 1024);
            try (FileChannel ch = FileChannel.open(t, StandardOpenOption.READ,
                    StandardOpenOption.WRITE)) {
                ch.truncate(30000);
                ok("truncate size", ch.size() == 30000, "size=" + ch.size());
                ByteBuffer bb = ByteBuffer.allocate(8);
                int r = ch.read(bb, 30000);
                ok("read at new EOF", r <= 0, "read returned " + r + " past the new EOF");
                ByteBuffer pre = ByteBuffer.allocate(30000);
                int off = 0;
                while (pre.hasRemaining()) {
                    int k = ch.read(pre, off);
                    if (k < 0) break;
                    off += k;
                }
                ok("truncate keeps prefix", same(pre.array(), srcBytes, 0, 0, 30000),
                   "prefix mismatch=" + firstDiff(pre.array(), srcBytes, 30000));
            }
            ok("size after close", Files.size(t) == 30000, "on-disk size=" + Files.size(t));
        });

        check("truncate to a larger size is a no-op", () -> {
            Path t = path("t-trunc2.dat");
            seed(t, 20000);
            try (FileChannel ch = FileChannel.open(t, StandardOpenOption.READ,
                    StandardOpenOption.WRITE)) {
                ch.truncate(40000);
                ok("truncate grow is no-op", ch.size() == 20000, "size=" + ch.size());
            }
        });

        check("setLength grows (ftruncate extend)", () -> {
            Path t = path("t-grow.dat");
            seed(t, 20000);
            try (RandomAccessFile raf = new RandomAccessFile(t.toFile(), "rw")) {
                raf.setLength(40000);
                ok("setLength size", raf.length() == 40000, "length=" + raf.length());
                raf.seek(20000);
                ok("extended tail reads zero", raf.read() == 0,
                   "first byte of the extension is not zero");
                raf.seek(39999);
                ok("last extended byte readable", raf.read() == 0,
                   "last byte of the extension is not zero");
            }
            ok("extended on-disk size", Files.size(t) == 40000,
               "on-disk size=" + Files.size(t));
            byte[] got = slurp(t);
            ok("extend keeps prefix", same(got, srcBytes, 0, 0, 20000),
               "prefix mismatch=" + firstDiff(got, srcBytes, 20000));
        });

        /* ---------------- positional I/O ---------------- */
        check("positional write does not move the cursor", () -> {
            Path p = path("t-pos.dat");
            seed(p, 4096);
            try (FileChannel ch = FileChannel.open(p, StandardOpenOption.READ,
                    StandardOpenOption.WRITE)) {
                ch.position(1000);
                byte[] w = { 'P', 'O', 'S', 'I', 'X' };
                int wn = ch.write(ByteBuffer.wrap(w), 2048);
                ok("positional write count", wn == w.length, "wrote " + wn);
                ok("cursor unchanged", ch.position() == 1000, "position=" + ch.position());
                ByteBuffer rb = ByteBuffer.allocate(5);
                ch.read(rb, 2048);
                String s = new String(rb.array(), "ISO-8859-1");
                ok("positional write landed", s.equals("POSIX"), "read back '" + s + "'");
                ch.force(false);
            }
            byte[] got = slurp(p);
            ok("positional write on disk", same(got, "POSIX".getBytes("ISO-8859-1"), 2048, 0, 5),
               "byte at 2048 on disk = 0x" + String.format("%02x", got[2048] & 0xff));
        });

        check("positional read at and past EOF", () -> {
            Path p = path("t-eof.dat");
            seed(p, 1000);
            try (FileChannel ch = FileChannel.open(p, StandardOpenOption.READ)) {
                ByteBuffer bb = ByteBuffer.allocate(16);
                int atEnd = ch.read(bb, 1000);
                int past = ch.read(ByteBuffer.allocate(16), 5000);
                ok("read at EOF returns -1", atEnd == -1, "returned " + atEnd);
                ok("read past EOF returns -1", past == -1, "returned " + past);
            }
        });

        System.out.println("SUMMARY pass=" + pass + " fail=" + fail);
        System.out.println("TRANSFER-PROBE: DONE");
    }

    static String firstDiff(byte[] got, byte[] want, int len) {
        for (int i = 0; i < len; i++) {
            int g = (i < got.length) ? (got[i] & 0xff) : -1;
            int w = (i < want.length) ? (want[i] & 0xff) : -1;
            if (g != w) return i + " got=" + g + " want=" + w;
        }
        return "none";
    }
}
