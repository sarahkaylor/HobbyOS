/* tools/rw/mono-probe/min6.cs — is nested PLINQ in the guest *hung* or just
 * ~100x slow?  min5 measured: a 4-element nested query 140 ms alone but ~3000 ms
 * inside a query body, and 100 of them did not finish in 15 s.  This probe
 * counts completions and prints progress, so steady progress (slow) and a
 * frozen counter (hung) are different rows.
 */
using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

class M6 {
  static readonly int[] src300k = new int[300000];
  static int done;
  static long sink;

  static void Row(string name, Func<object> f, int timeoutMs) {
    int t0 = Environment.TickCount;
    Task<object> t = Task.Factory.StartNew(f, CancellationToken.None,
                                           TaskCreationOptions.None, TaskScheduler.Default);
    bool ok = t.Wait(timeoutMs);
    Console.WriteLine("ROW " + name + " done=" + ok + " ms=" + (Environment.TickCount - t0) +
                      " status=" + t.Status);
  }

  static void Main(string[] argv) {
    for (int i = 0; i < src300k.Length; i++) src300k[i] = i;
    int to = argv.Length > 0 ? int.Parse(argv[0]) : 12000;
    int budget = argv.Length > 1 ? int.Parse(argv[1]) : 150000;
    Console.WriteLine("MIN6 start cpus=" + Environment.ProcessorCount +
                      " to=" + to + " budget=" + budget);
    Task[] w = new Task[8];
    for (int i = 0; i < 8; i++) w[i] = Task.Factory.StartNew(() => Thread.Sleep(20));
    Console.WriteLine("WARM ok=" + Task.WaitAll(w, 120000));

    // 40 outer elements, each body runs one nested PLINQ aggregate.
    Task t = Task.Factory.StartNew(() => {
      int[] r = src300k.AsParallel().WithDegreeOfParallelism(4)
                       .Select(x => {
                         if (x < 40) {
                           long s = src300k.Take(64).AsParallel().Sum(y => y + x);
                           Interlocked.Increment(ref done);
                           return (int)s;
                         }
                         return x;
                       }).ToArray();
      return r.Length;
    }, CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default);

    int t0 = Environment.TickCount;
    for (int i = 0; i < budget / 1000; i++) {
      if (t.Wait(1000)) { Console.WriteLine("PROGRESS finished at " + (Environment.TickCount - t0) + " ms"); break; }
      Console.WriteLine("PROGRESS t=" + (Environment.TickCount - t0) + "ms nested_done=" +
                        Volatile.Read(ref done) + " status=" + t.Status);
    }
    Console.WriteLine("ROW nested40 done=" + t.IsCompleted + " nested_done=" + done +
                      " ms=" + (Environment.TickCount - t0) + " status=" + t.Status);
    Console.WriteLine("MIN6-OK");
  }
}
