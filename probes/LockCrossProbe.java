/*
 * probes/LockCrossProbe.java -- the CROSS-PROCESS half of the lock probe.
 *
 * This class runs in a boot whose /WAVE.CFG has TWO lines:
 *
 *     LOCKHOLD.EXE /JDKTMP/cross.dat 120000 /JDKTMP/cross.step -
 *     JDK/bin/java -cp /C LockCrossProbe
 *
 * Java cannot start a process on this kernel (clone() refuses fork/vfork --
 * see LOCK-PROBE lane 0), so the peer comes from the wave dispatcher, which
 * loads each manifest line as its own process.  That is a deliberate exception
 * to "one program per boot": a lock has to be contended by somebody else, and
 * this is the only way to have somebody else.  The consequence to keep in mind
 * when reading the result: two processes doing FAT16 I/O concurrently is the
 * configuration that wedges on proc_lock/fat_wr_lock on this TCG host, so a
 * TIMEOUT here is not by itself a lock bug -- check the same log for
 * [IDLESTUCK] / [YLOCK-WEDGE] before believing anything else in it.
 *
 * The peer (tools/jdk/lockhold.c) opens the file, takes F_WRLCK 0..EOF, prints
 * "LOCKHOLD: locked rc=0", then waits for /JDKTMP/cross.step.  On that cue it
 * runs F_GETLK, drops its own lock, and immediately retries F_SETLK -- so its
 * "relock rc=" line says whether anything of the JVM's was actually in the
 * kernel's lock table.
 *
 * What each outcome means for the JVM's blocking FileChannel.lock():
 *   - blocks until the peer releases (~120s)  -> F_SETLKW blocks, as POSIX says;
 *   - returns in milliseconds AND the peer's relock succeeds
 *                                             -> the kernel answered EAGAIN and
 *                                                the JDK manufactured a FileLock
 *                                                (it never checks for NO_LOCK on
 *                                                the blocking path).
 */
import java.io.*;
import java.nio.*;
import java.nio.channels.*;
import java.nio.file.*;

public class LockCrossProbe {
    static int pass = 0, fail = 0, n = 0;

    static void ok(String name, boolean cond, String detail) {
        int i = ++n;
        if (cond) { pass++; System.out.println("PASS " + i + " " + name); }
        else { fail++; System.out.println("FAIL " + i + " " + name + ": " + detail); }
    }

    static final String DIR = "/JDKTMP";
    static final Path F = Paths.get(DIR + "/cross.dat");
    static final Path STEP = Paths.get(DIR + "/cross.step");

    static FileChannel rw() throws IOException {
        return FileChannel.open(F, StandardOpenOption.CREATE,
                StandardOpenOption.READ, StandardOpenOption.WRITE);
    }

    public static void main(String[] a) throws Exception {
        System.out.println("LOCKX: begin (java " + System.getProperty("java.version") + ")");
        new File(DIR).mkdirs();
        try { Files.deleteIfExists(STEP); } catch (Throwable t) { /* fine */ }

        /* Wait for the peer's lock to become visible.  tryLock returning null is
         * the only in-Java evidence that a foreign owner exists -- the peer
         * prints to the same console but we cannot read its stdout from here. */
        FileChannel mc = null;
        FileLock probe = null;
        long waited = 0;
        while (waited < 60000) {
            try {
                if (mc == null) mc = rw();
                probe = mc.tryLock(0, Long.MAX_VALUE, false);
                if (probe == null) break;
                probe.release();
            } catch (Throwable t) {
                System.out.println("NOTE LOCKX open/tryLock retry: "
                                   + t.getClass().getSimpleName() + ": " + t.getMessage());
                if (mc != null) { try { mc.close(); } catch (Throwable x) { } mc = null; }
            }
            Thread.sleep(250);
            waited += 250;
        }
        System.out.println("NOTE LOCKX peer lock state after " + waited + "ms: "
                           + (probe == null ? "HELD by the peer" : "NOT HELD"));
        ok("LOCKX peer's exclusive lock is visible", probe == null,
           "tryLock succeeded after " + waited
           + "ms -- no foreign owner was ever in the kernel's table");

        if (mc == null) mc = rw();

        /* The blocking lock, timed. */
        long t0 = System.nanoTime();
        String outcome;
        FileLock blocking = null;
        try {
            blocking = mc.lock(0, Long.MAX_VALUE, false);
            outcome = "returned a FileLock (isValid=" + blocking.isValid()
                    + ", isShared=" + blocking.isShared() + ")";
        } catch (Throwable t) {
            outcome = t.getClass().getName() + ": " + t.getMessage();
        }
        long ms = (System.nanoTime() - t0) / 1_000_000;
        System.out.println("NOTE LOCKX contended lock() -> " + ms + "ms, " + outcome);
        ok("LOCKX blocking lock() waits for the peer", ms >= 5000,
           "returned after " + ms + "ms while the peer still held the lock: " + outcome);

        /* Release the peer so it can run its F_GETLK / relock sequence. */
        try { Files.write(STEP, new byte[] { 'x' }); }
        catch (Throwable t) { System.out.println("NOTE LOCKX step write: " + t); }

        /* Give the peer time to publish its transcript and exit. */
        long t1 = System.nanoTime();
        boolean freeAgain = false;
        while ((System.nanoTime() - t1) / 1_000_000 < 20000) {
            try {
                if (blocking != null && blocking.isValid()) blocking.release();
            } catch (Throwable t) { System.out.println("NOTE LOCKX release: " + t); }
            try {
                FileLock l = mc.tryLock(0, Long.MAX_VALUE, false);
                if (l != null) { l.release(); freeAgain = true; break; }
            } catch (Throwable t) {
                System.out.println("NOTE LOCKX retry: " + t);
                break;
            }
            Thread.sleep(300);
        }
        ok("LOCKX lock is free once the peer exits", freeAgain,
           "still refused 20s after the peer was released");

        try { mc.close(); } catch (Throwable t) { }
        System.out.println("SUMMARY pass=" + pass + " fail=" + fail);
        System.out.println("LOCKX: DONE");
    }
}
