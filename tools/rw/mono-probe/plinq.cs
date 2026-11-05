/* tools/rw/mono-probe/plinq.cs — the game's PLINQ shape, on a probe.
 *
 * The game's loader stops inside System.Linq.Parallel's merge:
 *   Monitor.Wait <- ManualResetEventSlim.Wait <- Task.SpinThenBlockingWait
 *   <- Task.InternalRunSynchronously <- SpoolingTask.SpoolStopAndGo
 *   <- DefaultMergeHelper.Execute <- MergeExecutor.Execute
 * with the awaited task never enqueued and its completion event never set
 * (docs/rimworld-launch-log.md 36.1/36.8).  This probe runs the same operator
 * families under the game's own Mono runtime so a host run and a guest run of
 * the same bytes separate "PLINQ is broken here" from "the game's own setup
 * differs".
 *
 * Each shape prints one line BEFORE and one AFTER, so a hang names the shape.
 */
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

class P {
  static int[] Src(int n) { int[] a = new int[n]; for (int i = 0; i < n; i++) a[i] = i; return a; }
  static int[] src = Src(300000);

  static void Shape(string name, Action a) {
    Console.WriteLine("SHAPE " + name + " start sched=" + TaskScheduler.Current.GetType().Name +
                      " sync=" + (SynchronizationContext.Current == null ? "(null)" :
                                  SynchronizationContext.Current.GetType().Name));
    int t0 = Environment.TickCount;
    a();
    Console.WriteLine("SHAPE " + name + " OK ms=" + (Environment.TickCount - t0));
  }

  // A Unity-like SynchronizationContext: Post() only runs when someone pumps.
  sealed class PumpCtx : SynchronizationContext {
    readonly Queue<KeyValuePair<SendOrPostCallback, object>> q =
        new Queue<KeyValuePair<SendOrPostCallback, object>>();
    public override void Post(SendOrPostCallback d, object st) {
      lock (q) q.Enqueue(new KeyValuePair<SendOrPostCallback, object>(d, st));
    }
    public int Pump() {
      int n = 0;
      while (true) {
        KeyValuePair<SendOrPostCallback, object> kv;
        lock (q) { if (q.Count == 0) break; kv = q.Dequeue(); }
        kv.Key(kv.Value); n++;
      }
      return n;
    }
  }

  static void Main() {
    Console.WriteLine("PLINQ-PROBE start cpus=" + Environment.ProcessorCount);

    Shape("where-toarray", () => {
      int[] r = src.AsParallel().Where(x => (x & 1) == 0).ToArray();
      if (r.Length != 150000) throw new Exception("len " + r.Length);
    });
    Shape("ordered-where-toarray", () => {
      int[] r = src.AsParallel().AsOrdered().Where(x => (x % 3) == 0).ToArray();
      if (r.Length != 100000 || r[1] != 3) throw new Exception("ordered " + r.Length);
    });
    Shape("select-tolist", () => {
      List<int> r = src.AsParallel().WithDegreeOfParallelism(4).Select(x => x * 2).ToList();
      if (r.Count != src.Length) throw new Exception("count");
    });
    Shape("sum-aggregate", () => {
      long s = src.AsParallel().Sum(x => (long)x);
      if (s != 44999850000L) throw new Exception("sum " + s);
    });
    Shape("inside-task-run", () => {
      Task<int[]> t = Task.Run(() => src.AsParallel().Where(x => (x & 3) == 0).ToArray());
      t.Wait();
      if (t.Result.Length != 75000) throw new Exception("nested len");
    });
    Shape("join-inside-query", () => {
      // The 36.8 shape: work inside the query blocks on a thread that has to
      // finish.  If a pool worker were consumed by the join, this is the
      // smallest query that reproduces it.
      Thread helper = new Thread(() => { Thread.Sleep(5); });
      helper.Start();
      int[] r = src.AsParallel().Where(x => { if (x == 0) helper.Join(); return x > 100; }).ToArray();
      if (r.Length != src.Length - 101) throw new Exception("join len " + r.Length);
    });
    Shape("sched-from-synccontext", () => {
      // The "scheduler only the blocked thread pumps" hypothesis: a PLINQ
      // merge started from a task on a SynchronizationContext scheduler.
      PumpCtx ctx = new PumpCtx();
      SynchronizationContext old = SynchronizationContext.Current;
      SynchronizationContext.SetSynchronizationContext(ctx);
      TaskScheduler sch = TaskScheduler.FromCurrentSynchronizationContext();
      Task<int[]> t = Task.Factory.StartNew(
          () => src.AsParallel().Where(x => (x & 7) == 0).ToArray(),
          CancellationToken.None, TaskCreationOptions.None, sch);
      int pumped = 0;
      while (!t.IsCompleted && pumped < 100000) pumped += ctx.Pump();
      SynchronizationContext.SetSynchronizationContext(old);
      if (!t.IsCompleted) throw new Exception("task never ran: pumped=" + pumped);
      if (t.Result.Length != 37500) throw new Exception("ctx len " + t.Result.Length);
      Console.WriteLine("  sched-from-synccontext pumps=" + pumped);
    });
    Shape("openmp-style-nested", () => {
      // A query whose body itself runs a query: recursion through the merge.
      int[] r = src.AsParallel().WithDegreeOfParallelism(8)
                   .Select(x => x < 1000 ? src.Take(64).AsParallel().Sum(y => y + x) : x)
                   .ToArray();
      if (r.Length != src.Length) throw new Exception("nested");
    });
    Console.WriteLine("PLINQ-OK");
  }
}
