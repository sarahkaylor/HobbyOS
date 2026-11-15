/*
 * probes/LockProbe.java -- advisory file locks (fcntl F_GETLK/F_SETLK/F_SETLKW)
 * as OpenJDK's FileChannel.lock()/tryLock() consumes them.
 *
 * TWO LANES, and they must not be confused with each other:
 *
 *   LANE 0 (can Java even make a second process?): FileChannel locks are
 *     per-PROCESS, so any real contention test needs one.  This lane pits
 *     ProcessBuilder against the kernel's clone(): if fork/vfork is refused,
 *     no cross-process test can be run from inside one JVM at all, and the
 *     lock question has to be asked from a two-program boot instead
 *     (probes/LockCrossProbe.java).
 *
 *   LANE 1 (in-JVM): two channels / two threads of ONE JVM.  POSIX record
 *     locks are owned by the process, and the JDK keeps its own FileLockTable
 *     per file key, so a second overlapping lock inside one JVM is refused by
 *     the JVM before any syscall happens.  Lane 1 therefore proves the JDK's
 *     bookkeeping and that nothing deadlocks -- it CANNOT prove anything about
 *     the kernel's lock table.
 */
import java.io.*;
import java.nio.*;
import java.nio.channels.*;
import java.nio.file.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public class LockProbe {
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

    static FileChannel rw(Path p) throws IOException {
        return FileChannel.open(p, StandardOpenOption.CREATE,
                StandardOpenOption.READ, StandardOpenOption.WRITE);
    }

    public static void main(String[] a) throws Exception {
        System.out.println("LOCK-PROBE: begin (java " + System.getProperty("java.version") + ")");
        new File(DIR).mkdirs();
        final Path f = path("lock.dat");
        Files.deleteIfExists(f);
        try (FileChannel ch = rw(f)) { ch.write(ByteBuffer.wrap(new byte[4096])); }

        /* ---------------- LANE 0: can we start a second process? ------------- */
        System.out.println("LANE0: ProcessBuilder (needed for any real contention test)");
        check("L0 ProcessBuilder can start a process", () -> {
            ProcessBuilder pb = new ProcessBuilder("/LOCKHOLD.EXE", f.toString(), "1000",
                                                   "-", "-");
            pb.redirectErrorStream(true);
            pb.redirectOutput(ProcessBuilder.Redirect.INHERIT);
            Process p = pb.start();
            boolean done = p.waitFor(20, TimeUnit.SECONDS);
            if (!done) { p.destroyForcibly(); throw new IOException("child did not exit"); }
            ok("L0 child exit code", p.exitValue() == 0, "rc=" + p.exitValue());
        });

        /* ---------------- LANE 1: one JVM ---------------- */
        System.out.println("LANE1: in-JVM (JDK FileLockTable + one-process kernel entries)");

        check("L1 tryLock exclusive", () -> {
            try (FileChannel ch = rw(f)) {
                FileLock l = ch.tryLock(0, Long.MAX_VALUE, false);
                ok("L1 tryLock returns a lock", l != null, "tryLock returned null");
                ok("L1 lock is exclusive", l != null && !l.isShared(), "isShared=true");
                ok("L1 lock is valid", l != null && l.isValid(), "isValid=false");
                if (l != null) l.release();
                ok("L1 release invalidates", l != null && !l.isValid(), "still valid");
            }
        });

        check("L1 blocking lock returns immediately", () -> {
            try (FileChannel ch = rw(f)) {
                long t0 = System.nanoTime();
                FileLock l = ch.lock(0, Long.MAX_VALUE, false);
                long ms = (System.nanoTime() - t0) / 1_000_000;
                ok("L1 lock() acquires", l != null, "lock() returned null");
                l.release();
                System.out.println("NOTE L1 uncontended lock() took " + ms + "ms");
            }
        });

        check("L1 shared lock on a read-only channel", () -> {
            try (FileChannel ch = FileChannel.open(f, StandardOpenOption.READ)) {
                FileLock l = ch.tryLock(0, Long.MAX_VALUE, true);
                ok("L1 shared tryLock", l != null && l.isShared(), "null or not shared");
                if (l != null) l.release();
                boolean threw = false;
                try { ch.tryLock(0, Long.MAX_VALUE, false); }
                catch (NonWritableChannelException e) { threw = true; }
                ok("L1 exclusive on read-only refused", threw,
                   "no NonWritableChannelException");
            }
        });

        check("L1 overlapping lock in one JVM is refused by the JDK", () -> {
            try (FileChannel a2 = rw(f); FileChannel b2 = rw(f)) {
                FileLock la = a2.lock(0, 1024, false);
                String got = "no exception";
                try {
                    FileLock lb = b2.tryLock(0, 1024, false);
                    got = "returned " + (lb == null ? "null" : "a lock");
                } catch (OverlappingFileLockException e) {
                    got = "OverlappingFileLockException";
                }
                la.release();
                ok("L1 second channel overlapping", got.equals("OverlappingFileLockException"),
                   got);
            }
        });

        check("L1 two threads, two channels", () -> {
            final Path g = path("lock-t.dat");
            Files.deleteIfExists(g);
            try (FileChannel seed = rw(g)) { seed.write(ByteBuffer.wrap(new byte[1024])); }
            final AtomicReference<String> second = new AtomicReference<>("<no result>");
            final CountDownLatch held = new CountDownLatch(1);
            final CountDownLatch done = new CountDownLatch(1);
            Thread t1 = new Thread(() -> {
                try (FileChannel ch = rw(g)) {
                    FileLock l = ch.lock(0, 1024, false);
                    held.countDown();
                    done.await(10, TimeUnit.SECONDS);
                    l.release();
                } catch (Throwable t) { second.set("t1: " + t); }
            });
            t1.start();
            held.await(10, TimeUnit.SECONDS);
            Thread t2 = new Thread(() -> {
                try (FileChannel ch = rw(g)) {
                    FileLock l = ch.tryLock(0, 1024, false);
                    second.set(l == null ? "null" : "acquired");
                    if (l != null) l.release();
                } catch (OverlappingFileLockException e) {
                    second.set("OverlappingFileLockException");
                } catch (Throwable t) { second.set(t.getClass().getName() + ": " + t.getMessage()); }
            });
            t2.start(); t2.join(15000); done.countDown(); t1.join(15000);
            ok("L1 two threads overlapping", "OverlappingFileLockException".equals(second.get()),
               "second thread got: " + second.get());
        });

        check("L1 lock past EOF is allowed", () -> {
            try (FileChannel ch = rw(f)) {
                FileLock l = ch.tryLock(1L << 20, 100, false);
                ok("L1 lock past EOF", l != null, "tryLock past EOF returned null");
                if (l != null) l.release();
            }
        });

        System.out.println("SUMMARY pass=" + pass + " fail=" + fail);
        System.out.println("LOCK-PROBE: DONE");
    }
}
