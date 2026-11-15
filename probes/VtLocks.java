/*
 * VtLocks.java -- monitor + park/unpark + j.u.c condition semantics.
 *
 * Everything here is a kernel futex conversation underneath; the point is to
 * find out which of them HobbyOS's Linux-ABI layer actually honours:
 *   synchronized contention  -> ObjectMonitor inflate + contend (native park)
 *   Object.wait/notify       -> monitor wait set (futex wait/requeue)
 *   LockSupport.park/unpark  -> raw Unsafe.park (futex direct)
 *   ReentrantLock/Condition  -> AQS (park/unpark), on platform threads AND VTs
 *   Semaphore                -> AQS shared mode (tryAcquire + park)
 *
 * Usage: VtLocks [all|check,...] [watchdogSeconds]
 */
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.concurrent.locks.*;

public class VtLocks {
    static int pass = 0, fail = 0, skip = 0, n = 0;
    static final Set<String> ran = new LinkedHashSet<>();
    static final Set<String> carriers = ConcurrentHashMap.newKeySet();

    interface Body { void run() throws Exception; }

    static String desc(Throwable t) {
        Throwable c = t;
        while (c.getCause() != null && c.getCause() != c) c = c.getCause();
        String m = c.toString();
        for (StackTraceElement s : c.getStackTrace())
            if (s.getClassName().startsWith("VtLocks")) { m += " @ " + s; break; }
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
        try { b.run(); ok(name, true, ""); }
        catch (Throwable t) { ok(name, false, desc(t)); }
        System.out.println("    (" + name + " took " + (System.nanoTime() - t0) / 1_000_000L + " ms)");
        System.out.flush();
    }

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
                for (Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) {
                    System.out.println("--- " + e.getKey().getName() + " [" + e.getKey() + "]");
                    for (StackTraceElement s : e.getValue()) System.out.println("      at " + s);
                }
            } catch (Throwable th) { System.out.println("dump failed: " + th); }
            System.out.println(">>> WATCHDOG halting VM");
            System.out.flush();
            Runtime.getRuntime().halt(9);
        }, "vtlocks-watchdog");
        t.setDaemon(true);
        t.start();
    }

    // ------------------------------------------------------------------ checks

    /* Baseline: 8 PLATFORM threads hammering one monitor.  If this fails, the
       monitor path is broken independent of virtual threads. */
    static void monPlatform() throws Exception {
        final Object lock = new Object();
        final int[] counter = {0};
        int T = 8, N = 50_000;
        Thread[] ts = new Thread[T];
        for (int i = 0; i < T; i++)
            ts[i] = new Thread(() -> { for (int j = 0; j < N; j++) synchronized (lock) { counter[0]++; } },
                    "pm-" + i);
        for (Thread t : ts) t.start();
        for (Thread t : ts) t.join(180_000);
        for (Thread t : ts) if (t.isAlive()) throw new IllegalStateException("platform thread alive");
        if (counter[0] != T * N) throw new IllegalStateException("counter=" + counter[0] + " != " + (T * N));
        System.out.println("    " + counter[0] + " contended increments exact on platform threads");
    }

    /* Same on 8 virtual threads: JEP 491 means a blocked monitor must UNMOUNT the
       VT, not pin the carrier. */
    static void monVirtual() throws Exception {
        final Object lock = new Object();
        final int[] counter = {0};
        int T = 8, N = 20_000;
        Thread[] ts = new Thread[T];
        for (int i = 0; i < T; i++)
            ts[i] = Thread.ofVirtual().name("vm-" + i).start(() -> {
                carriers.add(carrier());
                for (int j = 0; j < N; j++) synchronized (lock) { counter[0]++; }
            });
        for (Thread t : ts) t.join(180_000);
        for (Thread t : ts) if (t.isAlive()) throw new IllegalStateException("VT alive");
        if (counter[0] != T * N) throw new IllegalStateException("counter=" + counter[0] + " != " + (T * N));
        System.out.println("    " + counter[0] + " contended increments exact on virtual threads, carriers="
                + carriers.size());
    }

    /* Object.wait/notifyAll: the wait set lives in the inflated ObjectMonitor. */
    /* Bounded producer/consumer on ONE monitor: 2 producers, 4 consumers, a
       4-slot buffer, wait/notifyAll.  Every transfer is a wait-set entry and a
       wakeup, and the totals must come out exact. */
    static void waitNotify() throws Exception {
        final Object lock = new Object();
        final int ITEMS = 200, CAP = 4, PRODUCERS = 2, CONSUMERS = 4;
        final int[] buf = {0};
        final AtomicInteger got = new AtomicInteger();
        final int perProducer = ITEMS / PRODUCERS;
        Thread[] ps = new Thread[PRODUCERS];
        for (int i = 0; i < PRODUCERS; i++)
            ps[i] = new Thread(() -> {
                for (int k = 0; k < perProducer; k++) {
                    synchronized (lock) {
                        while (buf[0] >= CAP) {
                            try { lock.wait(5_000); } catch (InterruptedException e) { return; }
                        }
                        buf[0]++;
                        lock.notifyAll();
                    }
                }
            }, "wn-p" + i);
        Thread[] cs = new Thread[CONSUMERS];
        for (int i = 0; i < CONSUMERS; i++)
            cs[i] = new Thread(() -> {
                while (true) {
                    synchronized (lock) {
                        while (buf[0] == 0) {
                            if (got.get() >= ITEMS) return;
                            try { lock.wait(5_000); } catch (InterruptedException e) { return; }
                        }
                        buf[0]--;
                        got.incrementAndGet();
                        lock.notifyAll();
                    }
                }
            }, "wn-c" + i);
        for (Thread t : ps) t.start();
        for (Thread t : cs) t.start();
        for (Thread t : ps) { t.join(120_000); if (t.isAlive()) throw new IllegalStateException("producer alive, got=" + got.get()); }
        for (Thread t : cs) { t.join(120_000); if (t.isAlive()) throw new IllegalStateException("consumer alive, got=" + got.get()); }
        if (got.get() != ITEMS) throw new IllegalStateException("consumed=" + got.get() + " != " + ITEMS);
        synchronized (lock) { if (buf[0] != 0) throw new IllegalStateException("buffer left at " + buf[0]); }
        System.out.println("    " + got.get() + " items through a 4-slot monitor buffer, 2 producers / 4 consumers");
    }

    /* Timed wait must time out, and must return early on notify. */
    static void waitTimed() throws Exception {
        final Object lock = new Object();
        final AtomicLong timeoutMs = new AtomicLong(-1);
        Thread w = new Thread(() -> {
            synchronized (lock) {
                long t0 = System.nanoTime();
                try { lock.wait(300); } catch (InterruptedException e) { throw new RuntimeException(e); }
                timeoutMs.set((System.nanoTime() - t0) / 1_000_000L);
            }
        }, "wt");
        w.start();
        w.join(60_000);
        if (w.isAlive()) throw new IllegalStateException("timed waiter alive");
        long ms = timeoutMs.get();
        if (ms < 250) throw new IllegalStateException("wait(300) returned after " + ms + " ms (too early)");
        if (ms > 30_000) throw new IllegalStateException("wait(300) took " + ms + " ms (wildly late)");
        System.out.println("    wait(300) returned after " + ms + " ms");

        final Object lock2 = new Object();
        final AtomicLong notifiedMs = new AtomicLong(-1);
        Thread w2 = new Thread(() -> {
            synchronized (lock2) {
                long t0 = System.nanoTime();
                try { lock2.wait(60_000); } catch (InterruptedException e) { throw new RuntimeException(e); }
                notifiedMs.set((System.nanoTime() - t0) / 1_000_000L);
            }
        }, "wt2");
        w2.start();
        Thread.sleep(200);
        synchronized (lock2) { lock2.notifyAll(); }
        w2.join(60_000);
        if (w2.isAlive()) throw new IllegalStateException("notified waiter still alive after notifyAll");
        long nms = notifiedMs.get();
        if (nms > 5_000) throw new IllegalStateException("notify handoff took " + nms + " ms");
        System.out.println("    notifyAll woke the waiter after " + nms + " ms");
    }

    static void reentrant() throws Exception {
        final Object lock = new Object();
        Runnable deep = new Runnable() {
            public void run() {
                synchronized (lock) {
                    synchronized (lock) {
                        synchronized (lock) {
                            // deepest level: prove we are inside 3x
                        }
                    }
                }
            }
        };
        Thread.ofVirtual().start(deep).join(30_000);
        Thread p = new Thread(deep, "reent");
        p.start(); p.join(30_000);
        if (p.isAlive()) throw new IllegalStateException("reentrant holder alive");
        System.out.println("    nested synchronized x3 ok on VT and platform thread");
    }

    /* Raw Unsafe.park: no monitor, no j.u.c, just the futex layer. */
    static void parkPlatform() throws Exception {
        final Object blocker = new Object();
        final AtomicBoolean ordered = new AtomicBoolean();
        Thread t = new Thread(() -> {
            ordered.set(true);
            long t0 = System.nanoTime();
            LockSupport.parkNanos(blocker, 50_000_000L);   // 50 ms
            long ms = (System.nanoTime() - t0) / 1_000_000L;
            if (ms < 30) throw new IllegalStateException("parkNanos(50ms) returned after " + ms + " ms");
            // unpark BEFORE park must leave a permit that the next park consumes
            LockSupport.unpark(Thread.currentThread());
            long t1 = System.nanoTime();
            LockSupport.park(blocker);
            long ms2 = (System.nanoTime() - t1) / 1_000_000L;
            if (ms2 > 5_000) throw new IllegalStateException("pre-permit park blocked " + ms2 + " ms");
        }, "parker");
        t.start();
        t.join(60_000);
        if (t.isAlive()) throw new IllegalStateException("parker never woke: parkNanos(50ms) did not time out");
        if (!ordered.get()) throw new IllegalStateException("parker body never ran");
        System.out.println("    parkNanos timeouts and the unpark-before-park permit both work");
    }

    /* park/unpark handoff between two PLATFORM threads, 2000 rounds. */
    static void parkHandoff() throws Exception {
        final AtomicInteger turn = new AtomicInteger();
        final int ROUNDS = 2000;
        Thread[] ts = new Thread[2];
        for (int i = 0; i < 2; i++) {
            final int me = i;
            ts[i] = new Thread(() -> {
                for (int r = 0; r < ROUNDS; r++) {
                    while (turn.get() != me) LockSupport.park();
                    turn.set(1 - me);
                    LockSupport.unpark(ts[1 - me]);
                }
            }, "ho-" + i);
        }
        long t0 = System.nanoTime();
        ts[0].start(); ts[1].start();
        for (Thread t : ts) { t.join(120_000); if (t.isAlive()) throw new IllegalStateException(t.getName() + " stuck at turn=" + turn.get()); }
        System.out.println("    " + ROUNDS + " park/unpark handoffs in "
                + (System.nanoTime() - t0) / 1_000_000L + " ms");
    }

    /* A virtual thread parked with an explicit blocker, woken by a platform
       thread: the unmount/remount path. */
    static void parkVirtual() throws Exception {
        final Object blocker = new Object();
        final AtomicBoolean woke = new AtomicBoolean();
        final AtomicReference<Thread> me = new AtomicReference<>();
        Thread vt = Thread.ofVirtual().name("vpark").start(() -> {
            me.set(Thread.currentThread());
            carriers.add(carrier());
            LockSupport.park(blocker);
            if (Thread.interrupted()) { /* cleared */ }
            woke.set(true);
        });
        long spin = System.currentTimeMillis() + 30_000;
        while (me.get() == null && System.currentTimeMillis() < spin) Thread.sleep(10);
        if (me.get() == null) throw new IllegalStateException("VT never started");
        Thread.sleep(200);
        Thread.State st = vt.getState();
        LockSupport.unpark(me.get());
        vt.join(60_000);
        if (vt.isAlive()) throw new IllegalStateException("unparked VT still alive (state was " + st + ")");
        if (!woke.get()) throw new IllegalStateException("VT did not reach the code after park");
        System.out.println("    VT parked (state " + st + "), platform thread unparked it, carrier=" + carriers);

        // unpark-before-park on a VT.  The VT must NOT be blocked in any other
        // parking call when the permit is granted (an intervening park would
        // legitimately consume it), so it spin-waits on a flag instead.
        final AtomicBoolean fast = new AtomicBoolean();
        final AtomicBoolean armed = new AtomicBoolean();
        final AtomicBoolean go = new AtomicBoolean();
        Thread vt2 = Thread.ofVirtual().name("vpre").start(() -> {
            armed.set(true);
            while (!go.get()) Thread.onSpinWait();
            long t0 = System.nanoTime();
            LockSupport.park();
            fast.set((System.nanoTime() - t0) / 1_000_000L < 5_000);
        });
        long armDeadline = System.currentTimeMillis() + 30_000;
        while (!armed.get() && System.currentTimeMillis() < armDeadline) Thread.sleep(1);
        if (!armed.get()) throw new IllegalStateException("vpre VT never armed");
        LockSupport.unpark(vt2);           // permit granted while the VT is RUNNING
        go.set(true);
        vt2.join(30_000);
        if (vt2.isAlive()) throw new IllegalStateException("pre-permit VT alive: park() did not consume the permit");
        if (!fast.get()) throw new IllegalStateException("unpark-before-park lost the permit on a VT");
    }

    static void aqsCondition() throws Exception {
        final ReentrantLock lock = new ReentrantLock(true);
        final Condition cond = lock.newCondition();
        final AtomicInteger served = new AtomicInteger();
        final int WAITERS = 4;
        Thread[] ts = new Thread[WAITERS];
        for (int i = 0; i < WAITERS; i++)
            ts[i] = Thread.ofVirtual().name("aqs-" + i).start(() -> {
                carriers.add(carrier());
                lock.lock();
                try {
                    while (served.get() == 0) {
                        try { cond.await(60, TimeUnit.SECONDS); }
                        catch (InterruptedException e) { return; }
                    }
                } finally { lock.unlock(); }
            });
        Thread.sleep(500);
        lock.lock();
        try { served.set(1); cond.signalAll(); } finally { lock.unlock(); }
        for (Thread t : ts) { t.join(60_000); if (t.isAlive()) throw new IllegalStateException("AQS waiter alive"); }
        // tryLock timeout against a lock held by ANOTHER thread (ReentrantLock
        // would acquire instantly for the holder, which is not what we test).
        ReentrantLock l2 = new ReentrantLock();
        Thread holder = new Thread(() -> {
            l2.lock();
            try { Thread.sleep(1_500); } catch (InterruptedException e) { }
            finally { l2.unlock(); }
        }, "l2-holder");
        holder.start();
        Thread.sleep(200);
        long t0 = System.nanoTime();
        boolean gotIt = l2.tryLock(200, TimeUnit.MILLISECONDS);
        long ms = (System.nanoTime() - t0) / 1_000_000L;
        if (gotIt) { l2.unlock(); throw new IllegalStateException("tryLock(200ms) succeeded on a held lock"); }
        if (ms < 150) throw new IllegalStateException("tryLock(200ms) gave up after " + ms + " ms");
        holder.join(30_000);
        if (holder.isAlive()) throw new IllegalStateException("lock holder alive");
        System.out.println("    Condition.await/signalAll woke " + WAITERS + " VTs; tryLock(200ms) waited " + ms + " ms");
    }

    /* Shared-mode AQS: 100 VTs through a 1-permit semaphore guarding a plain
       int.  A lost wakeup or a spurious release shows up as a wrong count. */
    static void semaphore() throws Exception {
        final Semaphore s = new Semaphore(1);
        final int[] plain = {0};
        int T = 100, N = 200;
        Thread[] ts = new Thread[T];
        for (int i = 0; i < T; i++)
            ts[i] = Thread.ofVirtual().start(() -> {
                carriers.add(carrier());
                for (int j = 0; j < N; j++) {
                    s.acquireUninterruptibly();
                    try { int v = plain[0]; plain[0] = v + 1; }
                    finally { s.release(); }
                }
            });
        for (Thread t : ts) { t.join(240_000); if (t.isAlive()) throw new IllegalStateException("semaphore VT alive, plain=" + plain[0]); }
        if (plain[0] != T * N) throw new IllegalStateException("plain=" + plain[0] + " != " + (T * N));
        if (s.availablePermits() != 1) throw new IllegalStateException("permits=" + s.availablePermits());
        System.out.println("    " + T * N + " guarded increments exact through a 1-permit Semaphore");
    }

    /* Pinning: JDK 24+ (JEP 491) must NOT pin on synchronized+.sleep, but
       Object.wait() inside synchronized still pins by specification. */
    static long sleepVts(int N, Object lock, Set<String> seen) throws Exception {
        Thread[] ts = new Thread[N];
        long t0 = System.nanoTime();
        for (int i = 0; i < N; i++) {
            ts[i] = Thread.ofVirtual().start(() -> {
                if (lock == null) {
                    seen.add(carrier());
                    try { Thread.sleep(100); } catch (InterruptedException e) { }
                    seen.add(carrier());
                } else {
                    // A monitor of this VT's OWN: mutual exclusion cannot be the
                    // reason a slow run happens, only pinning can.
                    Object mine = new Object();
                    synchronized (mine) {
                        seen.add(carrier());
                        try { Thread.sleep(100); } catch (InterruptedException e) { }
                        seen.add(carrier());
                    }
                }
            });
        }
        for (Thread t : ts) { t.join(180_000); if (t.isAlive()) throw new IllegalStateException("VT alive"); }
        return (System.nanoTime() - t0) / 1_000_000L;
    }

    /* JEP 491: synchronized + Thread.sleep must UNMOUNT, not pin.  Measured as a
       ratio against the same 64 VTs sleeping with no monitor at all; a pinned
       carrier serialises and the second number blows up. */
    static void pinSynchronized() throws Exception {
        final Object lock = new Object();   // non-null only selects the monitor arm
        int N = 64;
        Set<String> base = ConcurrentHashMap.newKeySet();
        long free = sleepVts(N, null, base);
        Set<String> held = ConcurrentHashMap.newKeySet();
        long sync = sleepVts(N, lock, held);
        System.out.println("    64 VTs x sleep(100): no monitor " + free + " ms (" + base.size()
                + " carriers), holding a monitor " + sync + " ms (" + held.size()
                + " carriers), parallelism=" + Runtime.getRuntime().availableProcessors());
        if (sync > Math.max(4 * free, free + 3_000))
            throw new IllegalStateException("monitor sleeps serialised: " + sync + " ms vs " + free + " ms unpinned");
    }

    /* Object.wait() inside synchronized still PINS a virtual thread (JEP 491
       left the wait set alone).  With more waiters than carriers that must
       deadlock, so this checks the boundary honestly at 4 waiters / 8 carriers. */
    static void vtObjectWait() throws Exception {
        final Object lock = new Object();
        final AtomicInteger woke = new AtomicInteger();
        int N = 4;
        Thread[] ts = new Thread[N];
        for (int i = 0; i < N; i++)
            ts[i] = Thread.ofVirtual().name("vw-" + i).start(() -> {
                synchronized (lock) {
                    try { lock.wait(60_000); } catch (InterruptedException e) { return; }
                    woke.incrementAndGet();
                }
            });
        Thread.sleep(1_000);
        Map<Thread.State, Integer> states = new TreeMap<>();
        for (Thread t : ts) states.merge(t.getState(), 1, Integer::sum);
        System.out.println("    VT states while in Object.wait: " + states + "; notifying");
        synchronized (lock) { lock.notifyAll(); }
        for (Thread t : ts) { t.join(60_000); if (t.isAlive()) throw new IllegalStateException("VT still in wait"); }
        if (woke.get() != N) throw new IllegalStateException("woke=" + woke.get());
        System.out.println("    all " + N + " VTs left the monitor wait set after notifyAll");
    }

    // -------------------------------------------------------------------- main

    public static void main(String[] args) throws Exception {
        String sel = args.length > 0 ? args[0] : "all";
        int wd = args.length > 1 ? Integer.parseInt(args[1]) : 300;
        if (sel.equals("all")) ran.add("all");
        else for (String s : sel.split(",")) ran.add(s.trim());
        System.out.println("=== VtLocks: java=" + System.getProperty("java.version")
                + " procs=" + Runtime.getRuntime().availableProcessors()
                + " tracePinned=" + System.getProperty("jdk.tracePinnedThreads", "(unset)")
                + " sel=" + sel);
        System.out.flush();
        watchdog(wd);

        if (want("monp")) check("monitor-platform-8x50k", VtLocks::monPlatform);
        if (want("monv")) check("monitor-virtual-8x20k", VtLocks::monVirtual);
        if (want("wn")) check("obj-wait-notifyall", VtLocks::waitNotify);
        if (want("wt")) check("obj-wait-timed", VtLocks::waitTimed);
        if (want("reent")) check("monitor-reentrant", VtLocks::reentrant);
        if (want("park")) check("park-nanos-and-permit", VtLocks::parkPlatform);
        if (want("ho")) check("park-handoff-2000", VtLocks::parkHandoff);
        if (want("vpark")) check("vt-park-unpark", VtLocks::parkVirtual);
        if (want("aqs")) check("aqs-condition-trylock", VtLocks::aqsCondition);
        if (want("sem")) check("semaphore-mutual-exclusion", VtLocks::semaphore);
        if (want("pin")) check("vt-sync-across-sleep", VtLocks::pinSynchronized);
        if (want("vwait")) check("vt-object-wait", VtLocks::vtObjectWait);

        System.out.println("SUMMARY pass=" + pass + " fail=" + fail + " skip=" + skip);
        System.out.flush();
    }

    static boolean want(String k) { return ran.contains("all") || ran.contains(k); }
}
