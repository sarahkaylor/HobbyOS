/* tools/rw/mono-probe/min11.cs — split the ~12 ms threadpool round trip.
 *
 * min10 showed every queue->run measurement at 10-15 ms in the guest (host:
 * 0 ms) even with the idle-CPU reschedule IPI and the sem_timedwait deadline
 * fix in place, and a native sem ping-pong at 0.37 ms, so the wait itself is
 * not the cost.  This probe timestamps the ITEM's first instruction:
 *
 *   queue_to_start = item start - queue time      (scheduler / unpark)
 *   start_to_seen  = main observed - item start   (post-item path)
 */
using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

class M11 {
  static double Ms(long a, long b) { return (b - a) * 1000.0 / Stopwatch.Frequency; }
  static void Stats(string tag, double[] v) {
    Array.Sort(v);
    double sum = 0; for (int i = 0; i < v.Length; i++) sum += v[i];
    Console.WriteLine("MIN11 " + tag + " n=" + v.Length +
                      " min=" + v[0].ToString("F2") +
                      " med=" + v[v.Length / 2].ToString("F2") +
                      " max=" + v[v.Length - 1].ToString("F2") +
                      " mean=" + (sum / v.Length).ToString("F2"));
  }

  static void Main() {
    Console.WriteLine("MIN11 start cpus=" + Environment.ProcessorCount);
    for (int i = 0; i < 5; i++) Task.Run(() => { }).Wait();

    int N = 20;
    double[] q2s = new double[N], s2o = new double[N], tot = new double[N];
    long[] started = new long[N];
    int[] done = new int[N];
    for (int i = 0; i < N; i++) {
      int k = i;
      long q0 = Stopwatch.GetTimestamp();
      ThreadPool.QueueUserWorkItem(_ => {
        Volatile.Write(ref started[k], Stopwatch.GetTimestamp());
        Volatile.Write(ref done[k], 1);
      });
      while (Volatile.Read(ref done[k]) == 0) Thread.SpinWait(20);
      long q1 = Stopwatch.GetTimestamp();
      q2s[i] = Ms(q0, Volatile.Read(ref started[k]));
      s2o[i] = Ms(Volatile.Read(ref started[k]), q1);
      tot[i] = Ms(q0, q1);
    }
    Stats("queue_to_start", q2s);
    Stats("start_to_seen", s2o);
    Stats("total", tot);

    // Same shape but the worker is already running an item: hold one worker
    // for 300 ms, queue a second item behind it.  If queue_to_start stays
    // ~12 ms the cost is not "wake a parked worker".
    ManualResetEventSlim gate = new ManualResetEventSlim(false);
    long g0 = Stopwatch.GetTimestamp();
    Task blocker = Task.Factory.StartNew(() => gate.Wait(5000),
        CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default);
    Thread.Sleep(50);
    long[] started2 = new long[N];
    int[] done2 = new int[N];
    double[] b2 = new double[N];
    for (int i = 0; i < N; i++) {
      int k = i;
      long q0 = Stopwatch.GetTimestamp();
      ThreadPool.QueueUserWorkItem(_ => {
        Volatile.Write(ref started2[k], Stopwatch.GetTimestamp());
        Volatile.Write(ref done2[k], 1);
      });
      while (Volatile.Read(ref done2[k]) == 0) Thread.SpinWait(20);
      b2[i] = Ms(q0, Volatile.Read(ref started2[k]));
    }
    Stats("busy_pool_queue_to_start", b2);
    gate.Set();
    blocker.Wait(6000);
    Console.WriteLine("MIN11-OK");
  }
}
