
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.concurrent.locks.*;

/** Threading and synchronisation under sustained load, in one JVM.
 *
 *  Every phase checks an INVARIANT, not just "it did not throw": a broken lock
 *  shows up as a wrong count, which is the only failure mode worth catching.
 *  Phases: synchronized, ReentrantLock, ReentrantReadWriteLock, Semaphore,
 *  CountDownLatch, CyclicBarrier, Phaser, wait/notify, ArrayBlockingQueue
 *  producer/consumer, AtomicLong/CAS, ConcurrentHashMap, interrupts, joins,
 *  and virtual threads doing the same monitor work as platform threads.
 */
public class ThreadStress {
  static int fails = 0;
  static void chk(String what, boolean ok, String detail) {
    System.out.println((ok ? "TS PASS " : "TS FAIL ") + what + (detail.isEmpty() ? "" : " " + detail));
    if (!ok) fails++;
  }

  public static void main(String[] a) throws Exception {
    long t0 = System.currentTimeMillis();
    final int T = 8, N = 60000;

    // 1. synchronized method on a shared object
    class Ctr { long v; synchronized void add() { v++; } synchronized long get() { return v; } }
    Ctr c = new Ctr();
    Thread[] ts = new Thread[T];
    for (int i = 0; i < T; i++) { ts[i] = new Thread(() -> { for (int k = 0; k < N; k++) c.add(); }); ts[i].start(); }
    for (Thread t : ts) t.join();
    chk("synchronized", c.get() == (long) T * N, "got=" + c.get() + " want=" + (long) T * N);

    // 2. ReentrantLock + tryLock contention
    ReentrantLock rl = new ReentrantLock();
    long[] rlv = {0};
    for (int i = 0; i < T; i++) { ts[i] = new Thread(() -> { for (int k = 0; k < N; k++) { rl.lock(); try { rlv[0]++; } finally { rl.unlock(); } } }); ts[i].start(); }
    for (Thread t : ts) t.join();
    chk("ReentrantLock", rlv[0] == (long) T * N, "got=" + rlv[0]);

    // 3. ReentrantReadWriteLock: readers must never see a torn value
    ReentrantReadWriteLock rw = new ReentrantReadWriteLock();
    AtomicInteger torn = new AtomicInteger();
    long[] pair = {0, 0}; // always equal
    Thread w = new Thread(() -> { for (int k = 0; k < 20000; k++) { rw.writeLock().lock(); try { pair[0]++; pair[1]++; } finally { rw.writeLock().unlock(); } } });
    Thread[] rs = new Thread[4];
    for (int i = 0; i < 4; i++) rs[i] = new Thread(() -> { for (int k = 0; k < 20000; k++) { rw.readLock().lock(); try { if (pair[0] != pair[1]) torn.incrementAndGet(); } finally { rw.readLock().unlock(); } } });
    w.start(); for (Thread r : rs) r.start();
    w.join(); for (Thread r : rs) r.join();
    chk("ReadWriteLock", torn.get() == 0 && pair[0] == 20000, "torn=" + torn.get() + " n=" + pair[0]);

    // 4. Semaphore as a concurrency limiter: never more than the permit count inside
    Semaphore sem = new Semaphore(3);
    AtomicInteger inside = new AtomicInteger(), maxInside = new AtomicInteger(), done = new AtomicInteger();
    for (int i = 0; i < T; i++) { final int id = i; ts[i] = new Thread(() -> { try { for (int k = 0; k < 2000; k++) { sem.acquire(); try { int n = inside.incrementAndGet(); maxInside.accumulateAndGet(n, Math::max); done.incrementAndGet(); } finally { inside.decrementAndGet(); sem.release(); } } } catch (InterruptedException e) {} }); ts[i].start(); }
    for (Thread t : ts) t.join();
    chk("Semaphore", done.get() == T * 2000 && maxInside.get() <= 3, "done=" + done.get() + " max=" + maxInside.get());

    // 5. CountDownLatch releases exactly once
    CountDownLatch start = new CountDownLatch(1), fin = new CountDownLatch(T);
    AtomicInteger ran = new AtomicInteger();
    for (int i = 0; i < T; i++) { ts[i] = new Thread(() -> { try { start.await(); ran.incrementAndGet(); } catch (InterruptedException e) {} fin.countDown(); }); ts[i].start(); }
    start.countDown(); boolean allWoke = fin.await(60, TimeUnit.SECONDS);
    chk("CountDownLatch", allWoke && ran.get() == T, "woke=" + ran.get() + "/" + T);

    // 6. CyclicBarrier reuse across generations
    CyclicBarrier bar = new CyclicBarrier(T);
    AtomicInteger gens = new AtomicInteger();
    for (int i = 0; i < T; i++) { ts[i] = new Thread(() -> { try { for (int k = 0; k < 200; k++) { bar.await(30, TimeUnit.SECONDS); gens.incrementAndGet(); } } catch (Exception e) { System.out.println("TS barrier ex " + e); } }); ts[i].start(); }
    for (Thread t : ts) t.join();
    chk("CyclicBarrier", gens.get() == T * 200, "crossings=" + gens.get() + " want=" + (T * 200));

    // 7. Phaser
    Phaser ph = new Phaser(1); AtomicInteger arrived = new AtomicInteger();
    for (int i = 0; i < T; i++) { ph.register(); final int id = i; ts[i] = new Thread(() -> { for (int k = 0; k < 100; k++) { arrived.incrementAndGet(); ph.arriveAndAwaitAdvance(); } ph.arriveAndDeregister(); }); ts[i].start(); }
    for (int k = 0; k < 100; k++) ph.arriveAndAwaitAdvance();
    ph.arriveAndDeregister(); for (Thread t : ts) t.join();
    chk("Phaser", arrived.get() == T * 100, "arrived=" + arrived.get());

    // 8. wait/notify handoff
    final Object mon = new Object(); final int[] hand = {0}; final AtomicInteger notified = new AtomicInteger();
    Thread cons = new Thread(() -> { for (int k = 0; k < 2000; k++) { synchronized (mon) { while (hand[0] == 0) { try { mon.wait(); } catch (InterruptedException e) { return; } } hand[0]--; notified.incrementAndGet(); } } });
    cons.start();
    for (int k = 0; k < 2000; k++) { synchronized (mon) { hand[0]++; mon.notify(); } }
    cons.join(120000);
    chk("wait/notify", notified.get() == 2000, "handed=" + notified.get());

    // 9. producer/consumer through ArrayBlockingQueue: nothing lost, nothing duplicated
    ArrayBlockingQueue<Integer> q = new ArrayBlockingQueue<>(64);
    Set<Integer> seen = ConcurrentHashMap.newKeySet();
    Thread prod = new Thread(() -> { for (int k = 0; k < 20000; k++) { try { q.put(k); } catch (InterruptedException e) { return; } } });
    Thread consu = new Thread(() -> { for (int k = 0; k < 20000; k++) { try { seen.add(q.take()); } catch (InterruptedException e) { return; } } });
    prod.start(); consu.start(); prod.join(); consu.join(120000);
    chk("ArrayBlockingQueue", seen.size() == 20000, "distinct=" + seen.size());

    // 10. AtomicLong CAS from many threads
    AtomicLong al = new AtomicLong();
    for (int i = 0; i < T; i++) { ts[i] = new Thread(() -> { for (int k = 0; k < N; k++) al.incrementAndGet(); }); ts[i].start(); }
    for (Thread t : ts) t.join();
    chk("AtomicLong", al.get() == (long) T * N, "got=" + al.get());

    // 11. ConcurrentHashMap.compute under contention
    ConcurrentHashMap<Integer, Integer> chm = new ConcurrentHashMap<>();
    for (int i = 0; i < T; i++) { ts[i] = new Thread(() -> { for (int k = 0; k < 20000; k++) chm.merge(k % 512, 1, Integer::sum); }); ts[i].start(); }
    for (Thread t : ts) t.join();
    long sum = chm.values().stream().mapToLong(Integer::longValue).sum();
    chk("ConcurrentHashMap", sum == (long) T * 20000, "sum=" + sum);

    // 12. interrupt wakes a blocked thread
    final CountDownLatch never = new CountDownLatch(1); final AtomicBoolean interrupted = new AtomicBoolean();
    Thread sleeper = new Thread(() -> { try { never.await(); } catch (InterruptedException e) { interrupted.set(true); } });
    sleeper.start(); Thread.sleep(150); sleeper.interrupt(); sleeper.join(30000);
    chk("interrupt", interrupted.get(), "flag=" + interrupted.get());

    // 13. the same monitor work on virtual threads
    ReentrantLock vl = new ReentrantLock(); long[] vv = {0};
    Thread[] vts = new Thread[200];
    for (int i = 0; i < vts.length; i++) { vts[i] = Thread.ofVirtual().start(() -> { for (int k = 0; k < 2000; k++) { vl.lock(); try { vv[0]++; } finally { vl.unlock(); } } }); }
    for (Thread t : vts) t.join();
    chk("virtual-thread lock", vv[0] == 200L * 2000, "got=" + vv[0]);

    long ms = System.currentTimeMillis() - t0;
    System.out.println("TS DONE ms=" + ms + " fail=" + fails);
    System.out.println("TS VERDICT " + (fails == 0 ? "PASS" : "FAIL"));
  }
}
