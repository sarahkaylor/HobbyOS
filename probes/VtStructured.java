/*
 * VtStructured.java -- StructuredTaskScope (preview) on HobbyOS.
 *
 * Preview APIs are version-locked: a class file compiled by a *different*
 * release's javac is rejected, so this one is compiled INSIDE the guest by
 * /JDK/bin/javac --enable-preview and then run by the guest JVM.
 *
 * Checks: fan-out with join, subtask state/result, failure propagation
 * (allSuccessfulOrThrow), and the scope timeout (cancellation of a sleeping
 * subtask -- which needs interrupt/unpark of a parked virtual thread to work).
 *
 * Usage: VtStructured [watchdogSeconds]
 */
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public class VtStructured {
    static int pass = 0, fail = 0, n = 0;

    interface Body { void run() throws Exception; }

    static void check(String name, Body b) {
        System.out.println(">>> BEGIN " + name);
        System.out.flush();
        long t0 = System.nanoTime();
        try {
            b.run();
            pass++; System.out.println("PASS " + (++n) + " " + name);
        } catch (Throwable t) {
            Throwable c = t;
            while (c.getCause() != null && c.getCause() != c) c = c.getCause();
            fail++; System.out.println("FAIL " + (++n) + " " + name + ": " + c);
        }
        System.out.println("    (" + name + " took " + (System.nanoTime() - t0) / 1_000_000L + " ms)");
        System.out.flush();
    }

    static void watchdog(int secs) {
        if (secs <= 0) return;
        Thread t = new Thread(() -> {
            try { Thread.sleep(secs * 1000L); } catch (InterruptedException e) { return; }
            System.out.println(">>> WATCHDOG fired after " + secs + "s -- dumping all threads");
            try {
                for (Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) {
                    System.out.println("--- " + e.getKey().getName() + " [" + e.getKey() + "]");
                    for (StackTraceElement s : e.getValue()) System.out.println("      at " + s);
                }
            } catch (Throwable th) { System.out.println("dump failed: " + th); }
            System.out.println(">>> WATCHDOG halting VM");
            System.out.flush();
            Runtime.getRuntime().halt(9);
        }, "vtscope-watchdog");
        t.setDaemon(true);
        t.start();
    }

    static void fanOut() throws Exception {
        int[] got = new int[3];
        try (var scope = StructuredTaskScope.open()) {
            var a = scope.fork(() -> { Thread.sleep(30); return 11; });
            var b = scope.fork(() -> { Thread.sleep(10); return 22; });
            var c = scope.fork(() -> { return 33; });
            scope.join();
            if (a.state() != StructuredTaskScope.Subtask.State.SUCCESS)
                throw new IllegalStateException("a.state=" + a.state());
            if (b.state() != StructuredTaskScope.Subtask.State.SUCCESS)
                throw new IllegalStateException("b.state=" + b.state());
            got[0] = a.get(); got[1] = b.get(); got[2] = c.get();
        }
        if (got[0] != 11 || got[1] != 22 || got[2] != 33)
            throw new IllegalStateException("results " + Arrays.toString(got));
    }

    static void failurePropagation() throws Exception {
        boolean threw = false;
        try (var scope = StructuredTaskScope.open()) {
            scope.fork(() -> { Thread.sleep(50); throw new IllegalStateException("boom-from-subtask"); });
            scope.fork(() -> { Thread.sleep(20_000); return 1; });
            try { scope.join(); }
            catch (Throwable e) {   // JDK 27 renamed the failure type; report what it is
                threw = true;
                System.out.println("    join() threw " + e.getClass().getName()
                        + " cause=" + e.getCause());
                if (e.getCause() == null
                        || !String.valueOf(e.getCause().getMessage()).contains("boom-from-subtask"))
                    throw new IllegalStateException("failure cause lost: " + e);
            }
        }
        if (!threw) throw new IllegalStateException("join() did not report the failing subtask");
    }

    static void timeout() throws Exception {
        long t0 = System.nanoTime();
        boolean cancelled = false;
        try (var scope = StructuredTaskScope.open(StructuredTaskScope.Joiner.<Integer>allSuccessfulOrThrow(),
                cfg -> cfg.withTimeout(Duration.ofMillis(300)))) {
            var s = scope.fork(() -> { Thread.sleep(20_000); return 1; });
            try { scope.join(); }
            catch (Throwable e) {
                cancelled = true;
                System.out.println("    join() threw " + e.getClass().getName()
                        + ", subtask state=" + s.state());
                if (s.state() == StructuredTaskScope.Subtask.State.SUCCESS)
                    throw new IllegalStateException("timed-out subtask reported SUCCESS");
            }
        }
        long ms = (System.nanoTime() - t0) / 1_000_000L;
        if (!cancelled) throw new IllegalStateException("timeout was not reported");
        if (ms > 60_000) throw new IllegalStateException("timeout took " + ms + " ms");
        System.out.println("    scope timed out after " + ms + " ms and cancelled the sleeping subtask");
    }

    public static void main(String[] args) throws Exception {
        int wd = args.length > 0 ? Integer.parseInt(args[0]) : 300;
        System.out.println("=== VtStructured: java=" + System.getProperty("java.version")
                + " vthreads=" + Thread.ofVirtual().name("probe").unstarted(() -> { }).isVirtual()
                + " wd=" + wd);
        System.out.flush();
        watchdog(wd);
        check("sts-fanout-join", VtStructured::fanOut);
        check("sts-failure-propagation", VtStructured::failurePropagation);
        check("sts-timeout-cancel", VtStructured::timeout);
        System.out.println("SUMMARY pass=" + pass + " fail=" + fail);
        System.out.flush();
    }
}
