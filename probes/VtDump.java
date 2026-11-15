/*
 * VtDump.java -- the "thread dumps consistent" half of the acceptance line.
 *
 * ThreadMXBean and Thread.getAllStackTraces() deliberately do not list virtual
 * threads (JEP 444), so the question that matters for HobbyOS is whether the
 * machinery a real dump uses works:
 *
 *   jdk.internal.vm.ThreadContainers  -- the tree jcmd walks to find VTs
 *   VirtualThreadSchedulerMXBean      -- scheduler parallelism + mounted count
 *   Thread.getStackTrace() on a parked VT
 *   Thread.dumpStack() from inside a VT
 *
 * Runs with:
 *   --add-exports java.base/jdk.internal.vm=ALL-UNNAMED --add-modules jdk.management
 */
import java.lang.management.ManagementFactory;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import jdk.internal.vm.ThreadContainer;
import jdk.internal.vm.ThreadContainers;
import jdk.management.VirtualThreadSchedulerMXBean;

public class VtDump {
    static int pass = 0, fail = 0, n = 0;

    interface Body { void run() throws Exception; }

    static void check(String name, Body b) {
        System.out.println(">>> BEGIN " + name);
        System.out.flush();
        long t0 = System.nanoTime();
        try { b.run(); pass++; System.out.println("PASS " + (++n) + " " + name); }
        catch (Throwable t) {
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
            System.out.println(">>> WATCHDOG fired after " + secs + "s");
            System.out.flush();
            Runtime.getRuntime().halt(9);
        }, "vtdump-watchdog");
        t.setDaemon(true);
        t.start();
    }

    static void containers() throws Exception {
        int N = 50;
        CountDownLatch ready = new CountDownLatch(N);
        CountDownLatch hold = new CountDownLatch(1);
        List<Thread> vts = new ArrayList<>();
        for (int i = 0; i < N; i++)
            vts.add(Thread.ofVirtual().name("vtd-" + i).start(() -> {
                ready.countDown();
                try { hold.await(); } catch (InterruptedException e) { }
            }));
        if (!ready.await(120, TimeUnit.SECONDS)) throw new IllegalStateException("VTs did not start");
        ThreadContainer root = ThreadContainers.root();
        long total = root.threadCount();
        int virt = 0, plat = 0;
        for (Thread t : root.threads().toList()) { if (t.isVirtual()) virt++; else plat++; }
        System.out.println("    ThreadContainers.root(): threadCount=" + total
                + " walked=" + (virt + plat) + " (virtual=" + virt + ", platform=" + plat + ")");
        hold.countDown();
        for (Thread t : vts) { t.join(60_000); if (t.isAlive()) throw new IllegalStateException("VT alive"); }
        if (virt < N) throw new IllegalStateException("container walk found " + virt + "/" + N + " virtual threads");
    }

    static void scheduler() throws Exception {
        VirtualThreadSchedulerMXBean mx =
                ManagementFactory.getPlatformMXBean(VirtualThreadSchedulerMXBean.class);
        if (mx == null) throw new IllegalStateException("no VirtualThreadSchedulerMXBean");
        System.out.println("    scheduler parallelism=" + mx.getParallelism()
                + " mounted=" + mx.getMountedVirtualThreadCount()
                + " queued=" + mx.getQueuedVirtualThreadCount()
                + " poolSize=" + mx.getPoolSize());
        if (mx.getParallelism() <= 0) throw new IllegalStateException("parallelism=" + mx.getParallelism());
    }

    static void parkedStack() throws Exception {
        Object gate = new Object();
        AtomicReference<Thread> self = new AtomicReference<>();
        CountDownLatch parkedNow = new CountDownLatch(1);
        Thread vt = Thread.ofVirtual().name("stacked").start(() -> {
            self.set(Thread.currentThread());
            parkedNow.countDown();
            java.util.concurrent.locks.LockSupport.park(gate);
        });
        if (!parkedNow.await(60, TimeUnit.SECONDS)) throw new IllegalStateException("VT never started");
        Thread.sleep(300);
        Thread t = self.get();
        StackTraceElement[] st = t.getStackTrace();
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < Math.min(st.length, 6); i++) sb.append("\n      at ").append(st[i]);
        System.out.println("    parked VT state=" + t.getState() + " frames=" + st.length + sb);
        if (t.getState() != Thread.State.WAITING)
            throw new IllegalStateException("parked VT state=" + t.getState());
        if (st.length == 0) throw new IllegalStateException("getStackTrace() returned nothing");
        boolean ours = false;
        for (StackTraceElement e : st) if (e.getMethodName().startsWith("lambda$parkedStack")) ours = true;
        if (!ours) throw new IllegalStateException("park frame missing from the stack");
        java.util.concurrent.locks.LockSupport.unpark(t);
        t.join(60_000);
        if (t.isAlive()) throw new IllegalStateException("VT alive after unpark");
    }

    static void dumpInsideVt() throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicInteger lines = new AtomicInteger();
        Thread vt = Thread.ofVirtual().name("dumper").start(() -> {
            System.out.println("    --- Thread.dumpStack() from inside a virtual thread ---");
            Thread.dumpStack();
            lines.incrementAndGet();
            done.countDown();
        });
        if (!done.await(60, TimeUnit.SECONDS)) throw new IllegalStateException("dumpStack never returned");
        vt.join(60_000);
        if (vt.isAlive()) throw new IllegalStateException("dumper VT alive");
        if (lines.get() != 1) throw new IllegalStateException("dumpStack did not complete");
    }

    public static void main(String[] args) throws Exception {
        int wd = args.length > 0 ? Integer.parseInt(args[0]) : 300;
        System.out.println("=== VtDump: java=" + System.getProperty("java.version")
                + " procs=" + Runtime.getRuntime().availableProcessors() + " wd=" + wd);
        System.out.flush();
        watchdog(wd);
        check("container-walk-finds-vts", VtDump::containers);
        check("scheduler-mxbean", VtDump::scheduler);
        check("parked-vt-stack-and-state", VtDump::parkedStack);
        check("thread-dumpstack-inside-vt", VtDump::dumpInsideVt);
        System.out.println("SUMMARY pass=" + pass + " fail=" + fail);
        System.out.flush();
    }
}
