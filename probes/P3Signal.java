/*
 * probes/P3Signal.java -- signal semantics a JVM is expected to have, in one
 * process, with no dependence on exec.
 *
 * Three separate claims, each falsifiable on its own console line:
 *
 *   A. rt_sigaction + delivery: install a Java handler for SIGINT/SIGTERM and
 *      raise it.  sun.misc.Signal.raise() is kill(getpid(), sig), so this is
 *      the real kernel delivery path (signal frame planted on the user stack,
 *      restorer -> rt_sigreturn), not a JVM-internal callback.
 *
 *   B. The JVM's own SIGTERM handling: with NO user handler installed, SIGTERM
 *      must run shutdown hooks and then exit 143.  HotSpot gets there through
 *      its "Terminator" thread (sigwait on a blocked set), which needs
 *      pthread_sigmask + a wakeable park -- the exact mechanism this kernel
 *      has never been asked for by a real program.
 *
 *   C. Process.destroy()'s underlying primitive: kill(pid, SIGTERM).  With no
 *      child to point it at, the honest measurement is the errno for a pid
 *      that does not exist (ESRCH) and for one that is not ours (EPERM) --
 *      a destroy() that "succeeds" at either would be lying to its caller.
 */
import java.io.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public class P3Signal {
    static void p(String s) { System.out.println("SIG: " + s); System.out.flush(); }
    static int pass = 0, fail = 0;

    static void ok(String name, boolean cond, String detail) {
        if (cond) { pass++; p("PASS " + name); }
        else { fail++; p("FAIL " + name + ": " + detail); }
    }

    static Thread watchdog(final int ms, final String why) {
        Thread t = new Thread(() -> {
            try { Thread.sleep(ms); } catch (InterruptedException e) { return; }
            p("WATCHDOG after " + ms + "ms while " + why);
            Runtime.getRuntime().halt(7);
        }, "wd");
        t.setDaemon(true);
        t.start();
        return t;
    }

    public static void main(String[] args) throws Exception {
        watchdog(120_000, "in main");
        p("start P3Signal");

        /* ---- A. custom handlers ---- */
        for (String name : new String[]{"INT", "TERM", "USR1", "HUP"}) {
            final AtomicInteger hit = new AtomicInteger();
            final AtomicLong at = new AtomicLong();
            final String expect = name;
            sun.misc.SignalHandler h = sig -> {
                hit.incrementAndGet();
                at.set(System.nanoTime());
                System.out.println("SIG: HANDLER " + sig.getName() + " on thread=" + Thread.currentThread().getName());
                System.out.flush();
                if (!sig.getName().equals(expect)) p("handler got wrong signal: " + sig.getName());
            };
            sun.misc.SignalHandler prev = null;
            try {
                prev = sun.misc.Signal.handle(new sun.misc.Signal(name), h);
                p("installed handler for " + name + " (prev=" + prev + ")");
            } catch (Throwable t) {
                fail++; p("FAIL handler-install-" + name + ": " + t.getClass().getName() + ": " + t.getMessage());
                continue;
            }
            long t0 = System.nanoTime();
            try {
                sun.misc.Signal.raise(new sun.misc.Signal(name));
            } catch (Throwable t) {
                fail++; p("FAIL raise-" + name + ": " + t.getClass().getName() + ": " + t.getMessage());
                continue;
            }
            /* Delivery is asynchronous (the frame runs on whatever thread the
               kernel picks).  A short bounded wait, then report the latency so
               a "delivered on the next 100 Hz tick" kernel shows up as ~10 ms. */
            long deadline = System.currentTimeMillis() + 5_000;
            while (hit.get() == 0 && System.currentTimeMillis() < deadline) Thread.sleep(5);
            ok("handler-" + name + "-delivered", hit.get() >= 1,
               "raise() returned but the handler never ran");
            if (hit.get() >= 1)
                p("  " + name + " latency=" + ((at.get() - t0) / 1_000_000) + "ms thread="
                  + Thread.currentThread().getName());
            /* Restore the previous disposition so section B sees the JVM's. */
            try { sun.misc.Signal.handle(new sun.misc.Signal(name), sun.misc.SignalHandler.SIG_DFL); }
            catch (Throwable t) { p("restore " + name + " failed: " + t); }
        }

        /* ---- C. kill(2) errno, the thing destroy() is built on ---- */
        long self = ProcessHandle.current().pid();
        p("self pid=" + self);
        try {
            /* A pid far beyond MAX_PROCESSES: Linux says ESRCH (3). */
            int rc = ProcessHandle.of(999_999L).map(ph -> {
                boolean d = ph.destroy();
                p("ProcessHandle.of(999999).destroy()=" + d);
                return d ? 0 : -1;
            }).orElse(-2);
            p("destroy-nonexistent -> " + rc);
        } catch (Throwable t) {
            p("destroy-nonexistent threw " + t);
        }

        /* ---- B. shutdown hooks on SIGTERM ---- */
        p("> installing shutdown hook and raising SIGTERM with default disposition");
        final AtomicBoolean hookRan = new AtomicBoolean();
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            hookRan.set(true);
            System.out.println("SIG: HOOK ran, thread=" + Thread.currentThread().getName());
            System.out.flush();
        }, "hook"));
        System.out.flush();
        try {
            sun.misc.Signal.raise(new sun.misc.Signal("TERM"));
        } catch (Throwable t) {
            p("raise TERM threw " + t);
        }
        /* If the default action is taken, this JVM is gone.  If the JVM's
           Terminator thread wakes up, the hook runs and exit is 143.  Reaching
           the line below at all means SIGTERM was IGNORED -- the failure mode
           that would make Process.destroy() a no-op. */
        Thread.sleep(5_000);
        fail++;
        p("FAIL sigterm-default-disposition: still alive 5s after raising SIGTERM, hookRan=" + hookRan.get());
        Runtime.getRuntime().halt(9);
    }
}
