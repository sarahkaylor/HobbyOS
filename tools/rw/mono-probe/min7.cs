/* tools/rw/mono-probe/min7.cs — the two questions a reproducer has to answer:
 *
 *  1. is nested PLINQ slow because every nested query needs a NEW pool worker
 *     (hill-climb injection)?  Run with /TPMIN8 (MONO_THREADS_PER_CPU=8) and
 *     the same shape: if it drops from ~500 ms per nested query to ~1 ms, the
 *     "hang" was pool latency, not a lost wake.
 *
 *  2. can a *properly ordered* Monitor.PulseAll ever be lost in the guest?
 *     The waiter publishes its phase under the lock and then waits; the pulser
 *     only pulses a phase it has seen published, so a lost pulse is a defect,
 *     not a legitimate race.  Several waiters, many rounds, a wall-clock
 *     watchdog: STORM-OK or STORM-HANG with the phase it stopped at.
 */
using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

class M7 {
  // ---- 2. ordered Monitor storm -------------------------------------------
  static void MonitorStorm(int rounds, int waiters) {
    object o = new object();
    int phase = 0;            // rounds completed (guarded by o)
    int published = 0;        // waiters parked for the current phase
    int stop = 0;
    Thread[] w = new Thread[waiters];
    for (int i = 0; i < waiters; i++) {
      w[i] = new Thread(() => {
        while (true) {
          lock (o) {
            if (Volatile.Read(ref stop) != 0) return;
            int mine = phase;
            published++;
            while (phase == mine && Volatile.Read(ref stop) == 0) Monitor.Wait(o);
            if (Volatile.Read(ref stop) != 0) return;
          }
        }
      });
      w[i].IsBackground = true;
      w[i].Start();
    }
    int t0 = Environment.TickCount;
    for (int r = 0; r < rounds; r++) {
      // wait until every waiter is parked for this phase, then pulse
      while (true) {
        lock (o) { if (published >= waiters) break; }
        if (Environment.TickCount - t0 > 30000) {
          Console.WriteLine("STORM-HANG phase=" + r + " published=" + published);
          Volatile.Write(ref stop, 1);
          return;
        }
        Thread.Yield();
      }
      lock (o) { phase++; published = 0; Monitor.PulseAll(o); }
    }
    Volatile.Write(ref stop, 1);
    lock (o) { Monitor.PulseAll(o); }
    bool all = true;
    foreach (Thread t in w) if (!t.Join(5000)) all = false;
    Console.WriteLine("STORM rounds=" + rounds + " waiters=" + waiters +
                      " ms=" + (Environment.TickCount - t0) + " joined=" + all);
  }

  // ---- 1. nested-query latency -------------------------------------------
  static void NestedLatency(int n, int budgetMs) {
    int[] src = new int[300000];
    for (int i = 0; i < src.Length; i++) src[i] = i;
    int done = 0;
    int t0 = Environment.TickCount;
    Task t = Task.Factory.StartNew(() => {
      src.AsParallel().WithDegreeOfParallelism(4).Select(x => {
        if (x < n) {
          long s = src.Take(64).AsParallel().Sum(y => y + x);
          Interlocked.Increment(ref done);
          return (int)s;
        }
        return x;
      }).ToArray();
    }, CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default);
    while (!t.Wait(1000)) {
      if (Environment.TickCount - t0 > budgetMs) break;
    }
    int ms = Environment.TickCount - t0;
    Console.WriteLine("NESTED n=" + n + " done=" + Volatile.Read(ref done) +
                      " ms=" + ms + " per_query_ms=" + (done > 0 ? ms / done : -1) +
                      " status=" + t.Status);
  }

  static void Main(string[] argv) {
    int rounds = argv.Length > 0 ? int.Parse(argv[0]) : 3000;
    Console.WriteLine("MIN7 start cpus=" + Environment.ProcessorCount +
                      " tp_per_cpu=" + (Environment.GetEnvironmentVariable("MONO_THREADS_PER_CPU") ?? "-"));
    MonitorStorm(rounds, 4);
    NestedLatency(20, 60000);
    MonitorStorm(rounds, 4);
    Console.WriteLine("MIN7-OK");
  }
}
