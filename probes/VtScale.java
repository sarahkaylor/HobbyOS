/*
 * VtScale.java -- N virtual threads, blocking, at scale.
 *
 * Phase A: N VTs that each sleep, then increment.  This is the "10k virtual
 *          threads run to completion" acceptance shape.
 * Phase B: N VTs contending on ONE bounded ArrayBlockingQueue (one lock, one
 *          Condition, N parked threads) with a single consumer VT.
 *
 * Usage: VtScale <N> [watchdogSeconds] [a|b|ab]
 * Prints wall-clock per phase, carrier count and heap use, so a boot tells you
 * how far the guest got even when it does not finish.
 */
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public class VtScale {
    static final Set<String> carriers = ConcurrentHashMap.newKeySet();

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
                int shown = 0;
                for (Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) {
                    if (shown++ > 60) { System.out.println("--- ... (" + (Thread.getAllStackTraces().size() - 60) + " more threads)"); break; }
                    System.out.println("--- " + e.getKey().getName() + " [" + e.getKey() + "]");
                    for (StackTraceElement s : e.getValue()) System.out.println("      at " + s);
                }
            } catch (Throwable th) { System.out.println("dump failed: " + th); }
            System.out.println(">>> WATCHDOG halting VM");
            System.out.flush();
            Runtime.getRuntime().halt(9);
        }, "vtscale-watchdog");
        t.setDaemon(true);
        t.start();
    }

    static long heapUsedMB() {
        Runtime r = Runtime.getRuntime();
        return (r.totalMemory() - r.freeMemory()) / (1024 * 1024);
    }

    static boolean phaseA(int N) throws Exception {
        System.out.println(">>> BEGIN scale-sleep N=" + N + " heap=" + heapUsedMB() + "MB");
        System.out.flush();
        AtomicInteger done = new AtomicInteger();
        List<Thread> ts = new ArrayList<>(N);
        long t0 = System.nanoTime();
        for (int i = 0; i < N; i++) {
            ts.add(Thread.ofVirtual().name("s" + i).start(() -> {
                carriers.add(carrier());
                try { Thread.sleep(20); } catch (InterruptedException e) { return; }
                done.incrementAndGet();
            }));
        }
        long createMs = (System.nanoTime() - t0) / 1_000_000L;
        System.out.println("    created " + ts.size() + " VTs in " + createMs + " ms");
        System.out.flush();
        long deadline = System.currentTimeMillis() + 240_000;
        int alive = 0;
        for (Thread t : ts) {
            long left = deadline - System.currentTimeMillis();
            if (left <= 0) { for (Thread x : ts) if (x.isAlive()) alive++; break; }
            t.join(left);
        }
        long ms = (System.nanoTime() - t0) / 1_000_000L;
        System.out.println("    A: " + done.get() + "/" + N + " finished, alive=" + alive
                + ", wall=" + ms + " ms, carriers=" + carriers.size()
                + ", heap=" + heapUsedMB() + "MB");
        System.out.flush();
        return done.get() == N;
    }

    static boolean phaseB(int N) throws Exception {
        System.out.println(">>> BEGIN scale-queue N=" + N + " heap=" + heapUsedMB() + "MB");
        System.out.flush();
        final BlockingQueue<Integer> q = new ArrayBlockingQueue<>(16);
        final long expect = (long) N * (N + 1) / 2;
        final AtomicLong sum = new AtomicLong();
        final CountDownLatch consumed = new CountDownLatch(1);
        long t0 = System.nanoTime();
        Thread consumer = Thread.ofVirtual().name("consumer").start(() -> {
            long s = 0;
            for (int i = 0; i < N; i++) {
                try { s += q.take(); } catch (InterruptedException e) { break; }
            }
            sum.set(s);
            consumed.countDown();
        });
        List<Thread> ts = new ArrayList<>(N);
        for (int i = 1; i <= N; i++) {
            final int v = i;
            ts.add(Thread.ofVirtual().name("p" + i).start(() -> {
                try { q.put(v); } catch (InterruptedException e) { }
            }));
        }
        long spawnMs = (System.nanoTime() - t0) / 1_000_000L;
        System.out.println("    producer VTs spawned in " + spawnMs + " ms");
        System.out.flush();
        boolean ok = consumed.await(240, TimeUnit.SECONDS);
        long ms = (System.nanoTime() - t0) / 1_000_000L;
        int prodsAlive = 0;
        for (Thread t : ts) if (t.isAlive()) prodsAlive++;
        System.out.println("    B: consumed latch=" + ok + ", sum=" + sum.get() + " (expect " + expect + ")"
                + ", producers alive=" + prodsAlive + ", consumer alive=" + consumer.isAlive()
                + ", wall=" + ms + " ms, heap=" + heapUsedMB() + "MB");
        System.out.flush();
        return ok && sum.get() == expect && prodsAlive == 0;
    }

    public static void main(String[] args) throws Exception {
        int N = args.length > 0 ? Integer.parseInt(args[0]) : 1000;
        int wd = args.length > 1 ? Integer.parseInt(args[1]) : 420;
        String phases = args.length > 2 ? args[2] : "ab";
        System.out.println("=== VtScale: java=" + System.getProperty("java.version")
                + " N=" + N + " procs=" + Runtime.getRuntime().availableProcessors()
                + " phases=" + phases + " maxHeap=" + (Runtime.getRuntime().maxMemory() >> 20) + "MB");
        System.out.flush();
        watchdog(wd);
        boolean a = true, b = true;
        if (phases.contains("a")) a = phaseA(N);
        if (phases.contains("b")) b = phaseB(N);
        System.out.println("SUMMARY a=" + (a ? "PASS" : "FAIL") + " b=" + (b ? "PASS" : "FAIL"));
        System.out.flush();
    }
}
