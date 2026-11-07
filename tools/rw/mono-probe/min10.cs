/* tools/rw/mono-probe/min10.cs — is the guest's per-task cost scheduling or wake?
 *
 * min9 measured ~146 ms for a plain `Task.Run(() => {}).Wait()` in the guest
 * (0 ms on the host, same bytes, 8 CPUs) and ~613 ms per nested PLINQ query
 * with the default pool vs ~286 ms with SetMinThreads(64).  That makes the
 * unit of cost one task round trip, not PLINQ.  This probe splits it:
 *
 *   A. warm Task.Run().Wait(): per-iteration distribution (min/med/max);
 *   B. scheduling only: queue an item that stores a flag, busy-spin until the
 *      flag -- no blocking wait at all;
 *   C. wake only: the item sets a ManualResetEventSlim, main blocks in Wait;
 *   D. same with a raw Monitor.Wait/Pulse pair (a managed condvar);
 *   E. ThreadPool.UnsafeQueueUserWorkItem + MRES (the Task path without Task).
 */
using System;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

class M10 {
  static double Ms(long a, long b) { return (b - a) * 1000.0 / Stopwatch.Frequency; }

  static void Stats(string tag, double[] v) {
    Array.Sort(v);
    double sum = 0; for (int i = 0; i < v.Length; i++) sum += v[i];
    Console.WriteLine("MIN10 " + tag + " n=" + v.Length + " min=" + v[0].ToString("F1") +
                      " med=" + v[v.Length / 2].ToString("F1") +
                      " max=" + v[v.Length - 1].ToString("F1") +
                      " mean=" + (sum / v.Length).ToString("F1"));
  }

  static void Main() {
    Console.WriteLine("MIN10 start cpus=" + Environment.ProcessorCount);
    // warm up
    for (int i = 0; i < 10; i++) Task.Run(() => { }).Wait();

    double[] a = new double[30];
    for (int i = 0; i < a.Length; i++) {
      long t0 = Stopwatch.GetTimestamp();
      Task.Run(() => { }).Wait();
      a[i] = Ms(t0, Stopwatch.GetTimestamp());
    }
    Stats("A task-wait warm", a);

    // B: scheduling only, busy-spin on a flag.  One flag per iteration so a
    // stale write cannot satisfy the next round.
    int[] flag = new int[30];
    double[] b = new double[flag.Length];
    for (int i = 0; i < flag.Length; i++) {
      int k = i;
      long t0 = Stopwatch.GetTimestamp();
      ThreadPool.QueueUserWorkItem(_ => Volatile.Write(ref flag[k], 1));
      while (Volatile.Read(ref flag[k]) == 0) Thread.SpinWait(20);
      b[i] = Ms(t0, Stopwatch.GetTimestamp());
    }
    Stats("B queue+spin", b);

    // C: queue + ManualResetEventSlim wake.
    double[] c = new double[30];
    for (int i = 0; i < c.Length; i++) {
      ManualResetEventSlim m = new ManualResetEventSlim(false);
      long t0 = Stopwatch.GetTimestamp();
      ThreadPool.QueueUserWorkItem(_ => m.Set());
      m.Wait();
      c[i] = Ms(t0, Stopwatch.GetTimestamp());
    }
    Stats("C queue+mres", c);

    // D: Monitor.Wait / Pulse across a pool worker.
    double[] d = new double[30];
    object o = new object();
    for (int i = 0; i < d.Length; i++) {
      int phase = i;
      int seen = 0;
      long t0 = Stopwatch.GetTimestamp();
      lock (o) {
        ThreadPool.QueueUserWorkItem(_ => { lock (o) { seen = phase + 1; Monitor.Pulse(o); } });
        while (seen != phase + 1) Monitor.Wait(o, 5000);
      }
      d[i] = Ms(t0, Stopwatch.GetTimestamp());
    }
    Stats("D queue+pulse", d);

    // E: UnsafeQueueUserWorkItem + MRES (Task's own path minus Task).
    double[] e = new double[30];
    for (int i = 0; i < e.Length; i++) {
      ManualResetEventSlim m = new ManualResetEventSlim(false);
      long t0 = Stopwatch.GetTimestamp();
      ThreadPool.UnsafeQueueUserWorkItem(_ => m.Set(), null);
      m.Wait();
      e[i] = Ms(t0, Stopwatch.GetTimestamp());
    }
    Stats("E unsafeq+mres", e);

    Console.WriteLine("MIN10-OK");
  }
}
