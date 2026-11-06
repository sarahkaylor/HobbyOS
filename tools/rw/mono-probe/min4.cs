/* tools/rw/mono-probe/min4.cs — the smallest nested-PLINQ shape.
 *
 * min3 showed that a PLINQ query started from inside a PLINQ worker never
 * completes in the guest (`Task.Status` stays Running for ever) while the same
 * code passes on the host in milliseconds, and that it fails even with
 * WithDegreeOfParallelism(1) and with free pool workers.  This probe drops the
 * element counts to single digits: if four elements over four elements still
 * hangs, the defect is in the nesting path itself, not in scale or timing.
 */
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

class M4 {
  static readonly int[] four = new int[] { 1, 2, 3, 4 };
  static readonly int[] big = new int[300000];

  static void Row(string name, Func<object> f, int timeoutMs) {
    Task<object> t = Task.Factory.StartNew(f, CancellationToken.None,
                                           TaskCreationOptions.None, TaskScheduler.Default);
    bool done = t.Wait(timeoutMs);
    string extra;
    if (done) {
      try { extra = " result=" + t.Result; } catch (Exception e) { extra = " EX=" + e.GetType().Name; }
    } else {
      extra = " status=" + t.Status;
    }
    Console.WriteLine("ROW " + name + " done=" + done + extra);
  }

  static void Main(string[] argv) {
    for (int i = 0; i < big.Length; i++) big[i] = i;
    int to = argv.Length > 0 ? int.Parse(argv[0]) : 5000;
    Console.WriteLine("MIN4 start cpus=" + Environment.ProcessorCount + " to=" + to);

    Row("solo-inner-4", () => four.AsParallel().Sum(), to);
    Row("solo-outer-4", () => four.AsParallel().Select(x => x + 1).ToArray().Length, to);
    Row("solo-outer-dop1-4", () => four.AsParallel().WithDegreeOfParallelism(1)
                                        .Select(x => x + 1).ToArray().Length, to);
    Row("tiny-nested-seq", () => four.AsParallel()
                                     .Select(x => four.Sum() + x).ToArray().Length, to);
    Row("tiny-nested-dop1", () => four.AsParallel().WithDegreeOfParallelism(1)
                                      .Select(x => four.AsParallel().Sum() + x).ToArray().Length, to);
    Row("tiny-nested-dop8", () => four.AsParallel().WithDegreeOfParallelism(8)
                                      .Select(x => four.AsParallel().Sum() + x).ToArray().Length, to);
    Row("tiny-nested-tolist", () => four.AsParallel()
                                       .Select(x => four.AsParallel().Where(y => y > 0).ToList().Count + x)
                                       .ToList().Count, to);
    Row("nested-in-task-only", () => Task.Factory.StartNew(
        () => four.AsParallel().Sum()).Result, to);
    Console.WriteLine("MIN4-OK");
  }
}
