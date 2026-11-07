/* tools/rw/mono-probe/min5.cs — nested-PLINQ probe with an explicit pool
 * warm-up, because the guest's ThreadPool can take >5 s to run its first work
 * item and that alone reads as "WaitingToRun" in a timed row.
 *
 * Measured question: does a PLINQ query started from inside a PLINQ worker
 * ever complete in the guest, once the pool is known to be alive?
 */
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

class M5 {
  static readonly int[] four = new int[] { 1, 2, 3, 4 };
  static int[] src;

  static bool Warm(int n, int timeoutMs) {
    Task[] t = new Task[n];
    for (int i = 0; i < n; i++) t[i] = Task.Factory.StartNew(() => Thread.Sleep(20));
    return Task.WaitAll(t, timeoutMs);
  }

  static void Row(string name, Func<object> f, int timeoutMs) {
    int t0 = Environment.TickCount;
    Task<object> t = Task.Factory.StartNew(f, CancellationToken.None,
                                           TaskCreationOptions.None, TaskScheduler.Default);
    bool done = t.Wait(timeoutMs);
    int ms = Environment.TickCount - t0;
    string extra;
    if (done) {
      try { extra = " result=" + t.Result; } catch (Exception e) { extra = " EX=" + e.GetType().Name; }
    } else {
      extra = " status=" + t.Status;
    }
    Console.WriteLine("ROW " + name + " done=" + done + " ms=" + ms + extra);
  }

  static void Main(string[] argv) {
    src = new int[300000];
    for (int i = 0; i < src.Length; i++) src[i] = i;
    int to = argv.Length > 0 ? int.Parse(argv[0]) : 15000;
    Console.WriteLine("MIN5 start cpus=" + Environment.ProcessorCount + " to=" + to);

    int w0 = Environment.TickCount;
    bool warm = Warm(8, 120000);
    Console.WriteLine("WARM ok=" + warm + " ms=" + (Environment.TickCount - w0));

    Row("solo-inner-4", () => four.AsParallel().Sum(), to);
    Row("solo-outer-dop1-4", () => four.AsParallel().WithDegreeOfParallelism(1)
                                        .Select(x => x + 1).ToArray().Length, to);
    Row("tiny-nested-seq", () => four.AsParallel()
                                     .Select(x => four.Sum() + x).ToArray().Length, to);
    Row("tiny-nested-dop1", () => four.AsParallel().WithDegreeOfParallelism(1)
                                      .Select(x => four.AsParallel().Sum() + x).ToArray().Length, to);
    Row("tiny-nested-dop8", () => four.AsParallel().WithDegreeOfParallelism(8)
                                      .Select(x => four.AsParallel().Sum() + x).ToArray().Length, to);
    Row("nested-64-dop1", () => src.AsParallel().WithDegreeOfParallelism(1)
                                    .Select(x => x < 100 ? src.Take(64).AsParallel().Sum(y => y + x) : x)
                                    .ToArray().Length, to);
    Row("solo-inner-4-again", () => four.AsParallel().Sum(), to);
    Console.WriteLine("MIN5-OK");
  }
}
