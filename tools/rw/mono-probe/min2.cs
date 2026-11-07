/* tools/rw/mono-probe/min2.cs — is the nested-PLINQ hang "the task never
 * completed" (pool starvation) or "the task completed and the waiter stayed
 * asleep" (the game's lost wake)?
 *
 * The answer is one row: Task.Wait(timeout) returning false while
 * Task.Status == RanToCompletion is exactly docs/rimworld-launch-log.md 37.2's
 * state (`st=...|ran`, `set=1 waiters=1`).  A task still Running /
 * WaitingForChildren means the pool never finished the work instead.
 *
 * Uses timeouts everywhere, so a hang prints state and the probe continues.
 */
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

class M2 {
  static int[] src;
  static int[] Src(int n) { int[] a = new int[n]; for (int i = 0; i < n; i++) a[i] = i; return a; }

  static void Pool(string tag) {
    int w, io, minw, minio, availw, availio;
    ThreadPool.GetMaxThreads(out w, out io);
    ThreadPool.GetMinThreads(out minw, out minio);
    ThreadPool.GetAvailableThreads(out availw, out availio);
    Console.WriteLine("  [" + tag + "] pool max=" + w + "/" + io + " min=" + minw +
                      " avail=" + availw + "/" + availio +
                      " procThreads=" + System.Diagnostics.Process.GetCurrentProcess().Threads.Count);
  }

  static ParallelQuery<int> Q(int dop) {
    ParallelQuery<int> q = src.AsParallel();
    if (dop > 0) q = q.WithDegreeOfParallelism(dop);
    return q;
  }

  // The exact shape, but awaited through a Task so its Status is readable.
  static void NestedQuery(int dop, int timeoutMs) {
    Task<int[]> outer = Task.Factory.StartNew(() =>
        Q(dop).Select(x => x < 1000 ? src.Take(64).AsParallel().Sum(y => y + x) : x)
           .ToArray(), CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default);
    bool done = outer.Wait(timeoutMs);
    Console.WriteLine("  nested dop=" + dop + " wait=" + done + " status=" + outer.Status +
                      " completed=" + outer.IsCompleted);
    if (!done) {
      Pool("stall");
      for (int i = 0; i < 3; i++) {
        Thread.Sleep(1000);
        Console.WriteLine("  +" + (i + 1) + "s status=" + outer.Status +
                          " completed=" + outer.IsCompleted + " faulted=" + outer.IsFaulted);
        Pool("stall");
      }
    } else {
      int[] r = outer.Result;
      if (r.Length != src.Length) throw new Exception("len " + r.Length);
    }
  }

  // Same query, but on a thread we own: Join(timeout) + the thread's state.
  static void NestedQueryThread(int dop, int timeoutMs) {
    int rc = -1;
    Thread t = new Thread(() => {
      int[] r = Q(dop).Select(x => x < 1000 ? src.Take(64).AsParallel().Sum(y => y + x) : x)
                   .ToArray();
      rc = r.Length;
    });
    t.IsBackground = true;
    t.Start();
    bool done = t.Join(timeoutMs);
    Console.WriteLine("  nested-thread dop=" + dop + " join=" + done + " len=" + rc +
                      " state=" + t.ThreadState);
    if (!done) Pool("thread-stall");
  }

  // A task that needs a pool thread while every worker is inside a nested
  // query: pure starvation test, with the completion observable.
  static void StarveProbe(int dop, int timeoutMs) {
    int n = dop;
    ManualResetEventSlim gate = new ManualResetEventSlim(false);
    List<Task> blockers = new List<Task>();
    for (int i = 0; i < n; i++) {
      int id = i;
      blockers.Add(Task.Factory.StartNew(() => {
        if (id == 0) gate.Set();
        gate.Wait();
        Thread.Sleep(2);
      }, CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default));
    }
    gate.Wait();
    Task<int> extra = Task.Factory.StartNew(() => 42, CancellationToken.None,
                                            TaskCreationOptions.None, TaskScheduler.Default);
    bool done = extra.Wait(timeoutMs);
    Console.WriteLine("  starve dop=" + dop + " extra done=" + done + " status=" + extra.Status);
    gate.Set();
  }

  static void Main(string[] argv) {
    src = Src(300000);
    int to = argv.Length > 0 ? int.Parse(argv[0]) : 20000;
    Console.WriteLine("MIN2 start cpus=" + Environment.ProcessorCount + " to=" + to);
    Pool("start");

    Console.WriteLine("shape nested-thread-dop8");
    NestedQueryThread(8, to);
    Console.WriteLine("shape nested-thread-dop2");
    NestedQueryThread(2, to);
    Console.WriteLine("shape nested-task-dop8");
    NestedQuery(8, to);
    Console.WriteLine("shape nested-task-dop2");
    NestedQuery(2, to);
    Console.WriteLine("shape starve-8");
    StarveProbe(8, to);
    Console.WriteLine("shape flat-dop8");
    NestedQuery(0, to);

    Console.WriteLine("MIN2-OK");
  }
}
