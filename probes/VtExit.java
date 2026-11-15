/*
 * VtExit.java -- virtual-thread teardown and edges.
 *
 * The park/wake layer is only half the story: the guest also has to survive
 * the END of a program full of virtual threads.
 *
 *   io       20 VTs reading real files at once (the kernel bounces user reads
 *            through one 32 KiB buffer per CPU, so this is a real stress of it)
 *   uncaught a VT that throws must reach its UncaughtExceptionHandler
 *   hook     a shutdown hook must run, with 100 VTs parked, when a VT calls
 *            System.exit(n)  (exit_group from a non-main thread)
 *
 * The exit code is printed as "WANT-EXIT=<n>" so the console log can be checked
 * against the kernel's own [EXITPC] line.
 *
 * Usage: VtExit <exitCode> [watchdogSeconds]
 */
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public class VtExit {
    static int pass = 0, fail = 0, n = 0;

    interface Body { void run() throws Exception; }

    static void check(String name, Body b) {
        System.out.println(">>> BEGIN " + name);
        System.out.flush();
        try { b.run(); pass++; System.out.println("PASS " + (++n) + " " + name); }
        catch (Throwable t) {
            Throwable c = t;
            while (c.getCause() != null && c.getCause() != c) c = c.getCause();
            fail++; System.out.println("FAIL " + (++n) + " " + name + ": " + c);
        }
        System.out.flush();
    }

    static void watchdog(int secs) {
        if (secs <= 0) return;
        Thread t = new Thread(() -> {
            try { Thread.sleep(secs * 1000L); } catch (InterruptedException e) { return; }
            System.out.println(">>> WATCHDOG fired after " + secs + "s -- dumping");
            try {
                for (Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) {
                    System.out.println("--- " + e.getKey().getName());
                    for (StackTraceElement s : e.getValue()) System.out.println("      at " + s);
                }
            } catch (Throwable th) { System.out.println("dump failed: " + th); }
            System.out.println(">>> WATCHDOG halting VM");
            System.out.flush();
            Runtime.getRuntime().halt(9);
        }, "vtexit-watchdog");
        t.setDaemon(true);
        t.start();
    }

    static void io() throws Exception {
        Path a = Paths.get("/JDK/release");
        Path b = Paths.get("/C/VtExit.class");
        long ea = Files.size(a), eb = Files.size(b);
        int T = 20;
        AtomicLong total = new AtomicLong();
        AtomicInteger bad = new AtomicInteger();
        Thread[] ts = new Thread[T];
        for (int i = 0; i < T; i++) {
            final boolean which = (i % 2) == 0;
            ts[i] = Thread.ofVirtual().name("io-" + i).start(() -> {
                try {
                    for (int k = 0; k < 5; k++) {
                        byte[] data = Files.readAllBytes(which ? a : b);
                        long want = which ? ea : eb;
                        if (data.length != want) bad.incrementAndGet();
                        total.addAndGet(data.length);
                    }
                } catch (Exception e) { bad.incrementAndGet(); }
            });
        }
        for (Thread t : ts) { t.join(180_000); if (t.isAlive()) throw new IllegalStateException("io VT alive"); }
        if (bad.get() != 0) throw new IllegalStateException("bad reads=" + bad.get());
        long expect = 5L * T / 2 * ea + 5L * T / 2 * eb;
        if (total.get() != expect) throw new IllegalStateException("bytes=" + total.get() + " != " + expect);
        System.out.println("    20 VTs x 5 reads of /JDK/release + /C/VtExit.class, all lengths exact ("
                + total.get() + " bytes)");
    }

    static void uncaught() throws Exception {
        AtomicReference<Throwable> seen = new AtomicReference<>();
        Thread vt = Thread.ofVirtual().name("thrower").unstarted(() -> {
            throw new IllegalStateException("expected-boom");
        });
        vt.setUncaughtExceptionHandler((t, e) -> seen.set(e));
        vt.start();
        vt.join(60_000);
        if (vt.isAlive()) throw new IllegalStateException("thrower alive");
        Throwable e = seen.get();
        if (e == null) throw new IllegalStateException("handler never fired");
        if (!String.valueOf(e.getMessage()).contains("expected-boom"))
            throw new IllegalStateException("handler saw " + e);
        System.out.println("    uncaught " + e + " delivered to the handler");
    }

    static void exitFromVt(int code) throws Exception {
        // parked VTs must not stop the exit, and the hook must run even though
        // the caller is a virtual thread (exit_group from a non-main thread).
        Object gate = new Object();
        for (int i = 0; i < 100; i++)
            Thread.ofVirtual().name("victim-" + i).start(() -> java.util.concurrent.locks.LockSupport.park(gate));
        Thread.sleep(500);
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            System.out.println("HOOK-RAN on " + Thread.currentThread().getName()
                    + " isVirtual=" + Thread.currentThread().isVirtual());
            System.out.flush();
        }, "hook"));
        Thread t = Thread.ofVirtual().name("exiter").start(() -> {
            try { Thread.sleep(200); } catch (InterruptedException e) { }
            System.out.println("WANT-EXIT=" + code + " (from virtual thread "
                    + Thread.currentThread().getName() + ")");
            System.out.flush();
            try { Thread.sleep(100); } catch (InterruptedException e) { }  // let the line flush
            System.exit(code);
        });
        t.join(30_000);
        // never reached
        throw new IllegalStateException("System.exit did not terminate the VM");
    }

    public static void main(String[] args) throws Exception {
        int code = args.length > 0 ? Integer.parseInt(args[0]) : 3;
        int wd = args.length > 1 ? Integer.parseInt(args[1]) : 300;
        System.out.println("=== VtExit: java=" + System.getProperty("java.version") + " code=" + code);
        System.out.flush();
        watchdog(wd);
        check("vt-concurrent-file-io", VtExit::io);
        check("vt-uncaught-handler", VtExit::uncaught);
        check("vt-system-exit", () -> exitFromVt(code));
        System.out.println("SUMMARY pass=" + pass + " fail=" + fail);
        System.out.flush();
    }
}
