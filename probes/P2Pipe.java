/*
 * probes/P2Pipe.java -- the pipe half of ProcessBuilder, tested WITHOUT a
 * second process.
 *
 * ProcessBuilder's three streams are pipes: the parent reads the child's
 * stdout, writes its stdin, and learns about EOF when the last writer closes.
 * Every one of those is pipe2(2) + read/write + close, so the kernel contract
 * can be measured in one JVM even while process creation is still broken --
 * and that is exactly the discrimination needed: "the child never started" and
 * "the pipe never delivered a byte" have the same symptom on the console.
 *
 * java.nio.channels.Pipe.open() is pipe2(O_CLOEXEC|O_NONBLOCK) on Linux, and
 * the streams are then made blocking by the JDK, which means this also proves
 * the blocking-read park/wake path the JDK's Process streams rely on.
 */
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.channels.*;
import java.util.concurrent.*;

public class P2Pipe {
    static void p(String s) { System.out.println("PIPE: " + s); System.out.flush(); }
    static int pass = 0, fail = 0;

    static void ok(String name, boolean cond, String detail) {
        if (cond) { pass++; p("PASS " + name); }
        else { fail++; p("FAIL " + name + ": " + detail); }
    }

    static Thread watchdog(final int ms) {
        Thread t = new Thread(() -> {
            try { Thread.sleep(ms); } catch (InterruptedException e) { return; }
            p("WATCHDOG after " + ms + "ms -- last step never returned");
            Runtime.getRuntime().halt(7);
        }, "wd");
        t.setDaemon(true);
        t.start();
        return t;
    }

    public static void main(String[] args) throws Exception {
        watchdog(120_000);

        /* 1. Round trip through a real pipe, more than one buffer's worth.
              PIPE_SIZE on this kernel is a fixed ring; a payload several times
              larger proves the write parks and the read wakes it rather than
              silently truncating. */
        {
            Pipe pipe = Pipe.open();
            final Pipe.SourceChannel rd = pipe.source();
            final Pipe.SinkChannel wr = pipe.sink();
            final byte[] payload = new byte[300_000];
            for (int i = 0; i < payload.length; i++) payload[i] = (byte) ('a' + (i % 26));
            Future<?> w = Executors.newSingleThreadExecutor(r -> {
                Thread t = new Thread(r, "writer"); t.setDaemon(true); return t;
            }).submit(() -> {
                try {
                    ByteBuffer bb = ByteBuffer.wrap(payload);
                    int off = 0;
                    while (bb.hasRemaining()) off += wr.write(bb);
                    wr.close();
                } catch (Throwable t) { p("writer failed: " + t); }
            });
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            ByteBuffer bb = ByteBuffer.allocate(8192);
            long t0 = System.currentTimeMillis();
            int n;
            while ((n = rd.read(bb)) > 0) { bb.flip(); bos.write(bb.array(), 0, n); bb.clear(); }
            long ms = System.currentTimeMillis() - t0;
            w.get(30, TimeUnit.SECONDS);
            byte[] got = bos.toByteArray();
            ok("pipe-roundtrip-300k", got.length == payload.length,
               "read " + got.length + " of " + payload.length);
            boolean same = got.length == payload.length;
            for (int i = 0; same && i < got.length; i++) if (got[i] != payload[i]) same = false;
            ok("pipe-bytes-exact", same, "payload corrupted");
            ok("pipe-eof-on-last-writer-close", n == -1, "final read returned " + n);
            p("roundtrip ms=" + ms);
        }

        /* 2. Blocking read on an empty pipe with a live writer: must PARK, not
              spin and not return 0 (a premature 0 is EOF, and the JDK's
              Process.getInputStream() turns that into a silent empty stream). */
        {
            Pipe pipe = Pipe.open();
            final Pipe.SourceChannel rd = pipe.source();
            final Pipe.SinkChannel wr = pipe.sink();
            final long[] wroteAt = new long[1];
            Thread t = new Thread(() -> {
                try {
                    Thread.sleep(1500);
                    wroteAt[0] = System.currentTimeMillis();
                    wr.write(ByteBuffer.wrap("late".getBytes("UTF-8")));
                } catch (Throwable e) { p("late writer: " + e); }
            });
            t.setDaemon(true);
            t.start();
            ByteBuffer bb = ByteBuffer.allocate(16);
            long t0 = System.currentTimeMillis();
            int n = rd.read(bb);
            long dt = System.currentTimeMillis() - t0;
            ok("pipe-blocking-read-waits", n == 4 && dt > 500,
               "n=" + n + " dt=" + dt + "ms");
            ok("pipe-blocking-read-payload",
               n == 4 && new String(bb.array(), 0, 4, "UTF-8").equals("late"),
               "n=" + n + " text=" + new String(bb.array(), 0, Math.max(n, 0)));
            t.join(10_000);
        }

        /* 3. EPIPE: writing with no reader left.  Linux raises SIGPIPE, which
              the JVM blocks; the write must come back as IOException "Broken
              pipe" rather than killing the JVM.  Same path a Process's stdin
              takes when the child dies without reading. */
        {
            Pipe pipe = Pipe.open();
            pipe.source().close();
            try {
                pipe.sink().write(ByteBuffer.wrap("x".getBytes("UTF-8")));
                fail++; p("FAIL pipe-epipe: write with no reader did not throw");
            } catch (IOException e) {
                pass++; p("PASS pipe-epipe: " + e.getClass().getName() + ": " + e.getMessage());
            }
        }

        p("SUMMARY pass=" + pass + " fail=" + fail);
        System.out.flush();
        Runtime.getRuntime().halt(fail == 0 ? 0 : 1);
    }
}
