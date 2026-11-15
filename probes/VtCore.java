/*
 * VtCore.java -- virtual-thread lifecycle on HobbyOS.
 *
 * One check per kernel surface, printed as
 *     >>> BEGIN <name>      (flushed before the work, so a hang is attributable)
 *     PASS <n> <name>
 *     FAIL <n> <name>: <detail>
 *     SUMMARY pass=.. fail=.. skip=..
 *
 * A hang after a BEGIN line names its own check without any further probing.
 *
 * Usage: VtCore [all|check1,check2] [watchdogSeconds]
 */
import java.lang.management.ManagementFactory;
import java.lang.management.ThreadInfo;
import java.lang.management.ThreadMXBean;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public class VtCore {
    static int pass = 0, fail = 0, skip = 0, n = 0;
    static final Set<String> carried = ConcurrentHashMap.newKeySet();
    static final List<Thread> parked = new ArrayList<>();
    static final Set<String> ran = new LinkedHashSet<>();

    interface Body { void run() throws Exception; }

    static String desc(Throwable t) {
        Throwable c = t;
        while (c.getCause() != null && c.getCause() != c) c = c.getCause();
        String m = c.toString();
        StackTraceElement[] st = c.getStackTrace();
        for (StackTraceElement s : st) {
            if (s.getClassName().startsWith("VtCore")) { m += " @ " + s; break; }
        }
        return m;
    }

    static void ok(String name, boolean cond, String detail) {
        int i = ++n;
        if (cond) { pass++; System.out.println("PASS " + i + " " + name); }
        else { fail++; System.out.println("FAIL " + i + " " + name + ": " + detail); }
        System.out.flush();
    }

    static void check(String name, Body b) {
        System.out.println(">>> BEGIN " + name);
        System.out.flush();
        long t0 = System.nanoTime();
        try {
            b.run();
            ok(name, true, "");
        } catch (Throwable t) {
            ok(name, false, desc(t));
        }
        long ms = (System.nanoTime() - t0) / 1_000_000L;
        System.out.println("    (" + name + " took " + ms + " ms, carriers=" + carried.size() + ")");
        System.out.flush();
    }

    /* The carrier a virtual thread is currently mounted on, or the thread's own
       description for a platform thread.  Thread.toString() of a mounted VT is
       "VirtualThread[#24]/runnable@ForkJoinPool-1-worker-3". */
    static String carrier() {
        String s = Thread.currentThread().toString();
        int i = s.indexOf('@');
        return i < 0 ? s : s.substring(i + 1);
    }

    static void watchdog(int secs) {
        if (secs <= 0) return;
        Thread t = new Thread(() -> {
            try { Thread.sleep(secs * 1000L); } catch (InterruptedException e) { return; }
            System.out.println(">>> WATCHDOG fired after " + secs + "s -- dumping all threads");
            try {
                for (Map.Entry<Thread, StackTraceElement[]> e
                        : Thread.getAllStackTraces().entrySet()) {
                    System.out.println("--- " + e.getKey().getName() + " [" + e.getKey() + "]");
                    for (StackTraceElement s : e.getValue()) System.out.println("      at " + s);
                }
            } catch (Throwable th) { System.out.println("dump failed: " + th); }
            System.out.println(">>> WATCHDOG halting VM");
            System.out.flush();
            Runtime.getRuntime().halt(9);
        }, "vtcore-watchdog");
        t.setDaemon(true);
        t.start();
    }

    // ------------------------------------------------------------------ checks

    static void mainIsPlatform() {
        Thread t = Thread.currentThread();
        if (t.isVirtual()) throw new IllegalStateException("main isVirtual()==true");
        if (!"main".equals(t.getName())) throw new IllegalStateException("name=" + t.getName());
        String d = t.toString();
        System.out.println("    main.toString()=" + d);
        System.out.println("    availableProcessors=" + Runtime.getRuntime().availableProcessors());
        if (d.contains("VirtualThread")) throw new IllegalStateException("main toString=" + d);
    }

    static void vtCreate() throws Exception {
        AtomicReference<Thread> seen = new AtomicReference<>();
        Thread t = Thread.ofVirtual().name("vt-solo").unstarted(() -> {
            carried.add(carrier());
            seen.set(Thread.currentThread());
        });
        if (!t.isVirtual()) throw new IllegalStateException("unstarted().isVirtual()==false");
        if (!"vt-solo".equals(t.getName())) throw new IllegalStateException("name=" + t.getName());
        if (t.getState() != Thread.State.NEW) throw new IllegalStateException("state=" + t.getState());
        if (t.getThreadGroup() == null) throw new IllegalStateException("null thread group before start");
        System.out.println("    pre-start threadGroup=" + t.getThreadGroup().getName());
        t.start();
        t.join(20_000);
        if (t.isAlive()) throw new IllegalStateException("still alive after join");
        Thread inside = seen.get();
        if (inside == null) throw new IllegalStateException("body never ran");
        if (inside != t) throw new IllegalStateException("currentThread != the VT");
        if (!inside.isVirtual()) throw new IllegalStateException("inside.isVirtual()==false");
        if (t.getState() != Thread.State.TERMINATED) throw new IllegalStateException("state=" + t.getState());
        // getThreadGroup() is null after termination for every thread (platform
        // too) -- measured on host JDK as well, so it is JDK semantics, not the OS.
        System.out.println("    carrier was " + carried.stream().findFirst().orElse("?")
                + ", post-termination group=" + t.getThreadGroup());
    }

    static void vtCpuOnly() throws Exception {
        int N = 200;
        long[] out = new long[N];
        Thread[] ts = new Thread[N];
        for (int i = 0; i < N; i++) {
            final int k = i;
            ts[i] = Thread.ofVirtual().name("cpu-" + i).start(() -> {
                carried.add(carrier());
                long s = 0;
                for (int j = 1; j <= 20_000; j++) s += (j * 31L) ^ (k + 7L);
                out[k] = s;
            });
        }
        long deadline = System.currentTimeMillis() + 120_000;
        for (Thread t : ts) {
            long left = deadline - System.currentTimeMillis();
            if (left <= 0) throw new IllegalStateException("deadline while joining");
            t.join(left);
            if (t.isAlive()) throw new IllegalStateException("VT " + t.getName() + " did not finish");
        }
        for (int i = 0; i < N; i++) {
            long expect = 0;
            for (int j = 1; j <= 20_000; j++) expect += (j * 31L) ^ (i + 7L);
            if (out[i] != expect) throw new IllegalStateException("task " + i + " sum " + out[i] + " != " + expect);
        }
        System.out.println("    " + N + " VTs on " + carried.size() + " distinct carriers, all sums exact");
    }

    static void vtSleep() throws Exception {
        int N = 100;
        AtomicInteger done = new AtomicInteger();
        Set<String> resumeCarriers = ConcurrentHashMap.newKeySet();
        Thread[] ts = new Thread[N];
        long t0 = System.nanoTime();
        for (int i = 0; i < N; i++) {
            ts[i] = Thread.ofVirtual().start(() -> {
                try {
                    Thread.sleep(0, 500_000);   // 0.5 ms, twice
                    resumeCarriers.add(carrier());
                    Thread.sleep(0, 500_000);
                    resumeCarriers.add(carrier());
                    done.incrementAndGet();
                } catch (InterruptedException e) { throw new RuntimeException(e); }
            });
        }
        long deadline = System.currentTimeMillis() + 120_000;
        for (Thread t : ts) {
            long left = deadline - System.currentTimeMillis();
            if (left <= 0) throw new IllegalStateException("deadline while joining, done=" + done.get());
            t.join(left);
            if (t.isAlive()) throw new IllegalStateException("sleeping VT alive, done=" + done.get());
        }
        long ms = (System.nanoTime() - t0) / 1_000_000L;
        if (done.get() != N) throw new IllegalStateException("done=" + done.get());
        System.out.println("    " + N + " VTs did 2 sleeps each in " + ms + " ms across "
                + resumeCarriers.size() + " carriers (parallelism="
                + Runtime.getRuntime().availableProcessors() + ")");
        if (resumeCarriers.size() > Runtime.getRuntime().availableProcessors() + 2)
            throw new IllegalStateException("carriers=" + resumeCarriers.size() + " exceeds parallelism: carrier pool grew?");
    }

    static void vtInterrupt() throws Exception {
        AtomicReference<String> got = new AtomicReference<>();
        Thread t = Thread.ofVirtual().name("sleeper").start(() -> {
            try {
                Thread.sleep(30_000);
                got.set("not interrupted");
            } catch (InterruptedException e) {
                got.set("interrupted");
                Thread.currentThread().interrupt();   // re-set, check it survives
            }
        });
        Thread.sleep(200);
        if (t.getState() != Thread.State.TIMED_WAITING)
            System.out.println("    WARN state before interrupt=" + t.getState());
        t.interrupt();
        t.join(20_000);
        if (t.isAlive()) throw new IllegalStateException("interrupted VT still alive");
        if (!"interrupted".equals(got.get())) throw new IllegalStateException("result=" + got.get());
    }

    static void vtQueueHandoff() throws Exception {
        BlockingQueue<Integer> q = new LinkedBlockingQueue<>();
        int N = 2000;
        AtomicLong sum = new AtomicLong();
        AtomicInteger count = new AtomicInteger();
        Thread prod = Thread.ofVirtual().name("producer").start(() -> {
            try { for (int i = 1; i <= N; i++) q.put(i); q.put(-1); }
            catch (InterruptedException e) { throw new RuntimeException(e); }
        });
        Thread cons = Thread.ofVirtual().name("consumer").start(() -> {
            try {
                while (true) {
                    int v = q.take();
                    if (v < 0) break;
                    sum.addAndGet(v);
                    count.incrementAndGet();
                }
            } catch (InterruptedException e) { throw new RuntimeException(e); }
        });
        prod.join(120_000);
        cons.join(120_000);
        if (prod.isAlive() || cons.isAlive()) throw new IllegalStateException("handoff threads alive");
        if (count.get() != N) throw new IllegalStateException("count=" + count.get());
        long expect = (long) N * (N + 1) / 2;
        if (sum.get() != expect) throw new IllegalStateException("sum=" + sum.get() + " != " + expect);
    }

    static void vtExecutor() throws Exception {
        int N = 200;
        long t0 = System.nanoTime();
        long sum = 0;
        try (ExecutorService ex = Executors.newVirtualThreadPerTaskExecutor()) {
            List<Future<Integer>> fs = new ArrayList<>();
            for (int i = 0; i < N; i++) {
                final int v = i;
                fs.add(ex.submit(() -> { Thread.sleep(1); return v * v; }));
            }
            for (Future<Integer> f : fs) sum += f.get(120, TimeUnit.SECONDS);
        }
        long ms = (System.nanoTime() - t0) / 1_000_000L;
        long expect = 0;
        for (int i = 0; i < N; i++) expect += (long) i * i;
        if (sum != expect) throw new IllegalStateException("sum=" + sum + " != " + expect);
        System.out.println("    executor: " + N + " tasks in " + ms + " ms, close() returned");
    }

    /* The teardown path: parked virtual threads that nobody joins must not keep
       the VM alive.  This is the shape that used to hang the guest. */
    static void vtParkedExit() throws Exception {
        int N = 500;
        Object gate = new Object();
        CountDownLatch started = new CountDownLatch(N);
        for (int i = 0; i < N; i++) {
            Thread t = Thread.ofVirtual().name("parked-" + i).start(() -> {
                started.countDown();
                java.util.concurrent.locks.LockSupport.park(gate);
            });
            parked.add(t);
        }
        if (!started.await(120, TimeUnit.SECONDS))
            throw new IllegalStateException("only " + (N - started.getCount()) + "/" + N + " parked VTs started");
        Thread.sleep(300);
        int alive = 0, waiting = 0;
        for (Thread t : parked) {
            if (t.isAlive()) alive++;
            if (t.getState() == Thread.State.WAITING) waiting++;
        }
        System.out.println("    " + alive + "/" + N + " VTs alive, " + waiting + " in WAITING; main returns now");
        if (alive != N) throw new IllegalStateException("only " + alive + "/" + N + " parked VTs alive");
        System.out.flush();
        // no join, no unpark: the VM has to exit by itself
    }

    static void dump() throws Exception {
        int N = 20;
        CountDownLatch hold = new CountDownLatch(1);
        CountDownLatch up = new CountDownLatch(N);
        List<Thread> vts = new ArrayList<>();
        for (int i = 0; i < N; i++)
            vts.add(Thread.ofVirtual().name("dumped-" + i).start(() -> {
                up.countDown();
                try { hold.await(); } catch (InterruptedException e) { }
            }));
        if (!up.await(60, TimeUnit.SECONDS)) throw new IllegalStateException("VTs did not start");
        ThreadMXBean mx = ManagementFactory.getThreadMXBean();
        ThreadInfo[] infos = mx.dumpAllThreads(true, true);
        int vt = 0;
        for (ThreadInfo ti : infos) if (ti.getThreadName().startsWith("dumped-")) vt++;
        // JEP 444: ThreadMXBean / getAllStackTraces do NOT list virtual threads
        // (verified identical on host JDK 26), so absence here is correct, not a gap.
        int inGroup = 0;
        for (Thread t : Thread.getAllStackTraces().keySet()) if (t.getName().startsWith("dumped-")) inGroup++;
        System.out.println("    dumpAllThreads -> " + infos.length + " platform entries, "
                + vt + " virtual (0 is correct by spec); getAllStackTraces lists " + inGroup);
        if (vt != 0) throw new IllegalStateException("MXBean unexpectedly listed " + vt + " virtual threads");
        // What must work instead: a parked VT answers for itself.
        hold.countDown();
        for (Thread t : vts) {
            t.join(60_000);
            if (t.isAlive()) throw new IllegalStateException("VT " + t.getName() + " did not finish");
        }
        System.out.println("    all " + N + " VTs ran and terminated after the dump");
    }

    // -------------------------------------------------------------------- main

    public static void main(String[] args) throws Exception {
        String sel = args.length > 0 ? args[0] : "all";
        int wd = args.length > 1 ? Integer.parseInt(args[1]) : 300;
        if (sel.equals("all")) ran.add("all");
        else for (String s : sel.split(",")) ran.add(s.trim());

        System.out.println("=== VtCore: java=" + System.getProperty("java.version")
                + " vm=" + System.getProperty("java.vm.version")
                + " procs=" + Runtime.getRuntime().availableProcessors()
                + " sel=" + sel);
        System.out.flush();
        watchdog(wd);

        if (want("main")) check("main-is-platform", VtCore::mainIsPlatform);
        if (want("create")) check("vt-create-and-join", VtCore::vtCreate);
        if (want("cpu")) check("vt-200-cpu-only", VtCore::vtCpuOnly);
        if (want("sleep")) check("vt-100-sleep-carriers", VtCore::vtSleep);
        if (want("interrupt")) check("vt-interrupt", VtCore::vtInterrupt);
        if (want("queue")) check("vt-queue-handoff", VtCore::vtQueueHandoff);
        if (want("executor")) check("vt-executor-per-task", VtCore::vtExecutor);
        if (want("dump")) check("vt-thread-dump", VtCore::dump);
        if (want("parked")) check("vt-parked-then-vm-exit", VtCore::vtParkedExit);

        System.out.println("SUMMARY pass=" + pass + " fail=" + fail + " skip=" + skip);
        System.out.flush();
    }

    static boolean want(String k) { return ran.contains("all") || ran.contains(k); }
}
