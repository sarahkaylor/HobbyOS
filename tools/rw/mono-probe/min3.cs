/* tools/rw/mono-probe/min3.cs — size/mode ladder for the guest-only PLINQ
 * non-completion found by min2.cs: every shape that runs a *nested* query
 * stays TaskStatus.Running for ever in the guest, while the same shapes pass
 * on the host in milliseconds.
 *
 * Every shape runs inside a Task and is awaited with a timeout, so one boot
 * produces the whole table instead of stopping at the first failure.
 */
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

class M3 {
  static int[] src;
  static int[] Src(int n) { int[] a = new int[n]; for (int i = 0; i < n; i++) a[i] = i; return a; }

  static void Row(string name, Func<object> f, int timeoutMs) {
    Task<object> t = Task.Factory.StartNew(f, CancellationToken.None,
                                           TaskCreationOptions.None, TaskScheduler.Default);
    bool done = t.Wait(timeoutMs);
    string extra = "";
    if (done) {
      try { extra = " result=" + t.Result; } catch (Exception e) { extra = " EX=" + e.GetType().Name; }
    } else {
      extra = " status=" + t.Status;
    }
    Console.WriteLine("ROW " + name + " done=" + done + extra);
  }

  static object SumN(int n) { long s = src.Take(n).AsParallel().Sum(x => (long)x); return s; }
  static object ToArrayN(int n) { int[] r = src.Take(n).AsParallel().Where(x => (x & 1) == 0).ToArray(); return r.Length; }
  static object SeqSum(int n) { long s = src.Take(n).Sum(x => (long)x); return s; }

  // outer query whose body runs a nested query
  static object NestedPar(int n, int dop) {
    int[] r = src.AsParallel().WithDegreeOfParallelism(dop)
                 .Select(x => x < 1000 ? src.Take(n).AsParallel().Sum(y => y + x) : x)
                 .ToArray();
    return r.Length;
  }
  // outer query whose body runs sequential LINQ only
  static object NestedSeq(int n, int dop) {
    int[] r = src.AsParallel().WithDegreeOfParallelism(dop)
                 .Select(x => x < 1000 ? src.Take(n).Sum(y => y + x) : x)
                 .ToArray();
    return r.Length;
  }
  // outer query, body does nothing parallel
  static object FlatOuter(int dop) {
    int[] r = src.AsParallel().WithDegreeOfParallelism(dop).Select(x => x + 1).ToArray();
    return r.Length;
  }

  static void Main(string[] argv) {
    src = Src(300000);
    int to = argv.Length > 0 ? int.Parse(argv[0]) : 8000;
    Console.WriteLine("MIN3 start cpus=" + Environment.ProcessorCount + " to=" + to);

    Row("seq-sum-64", () => SeqSum(64), to);
    Row("flat-outer-dop8-300k", () => FlatOuter(8), to);
    Row("par-sum-300k", () => SumN(300000), to);
    Row("par-sum-64", () => SumN(64), to);
    Row("par-sum-1024", () => SumN(1024), to);
    Row("par-sum-16384", () => SumN(16384), to);
    Row("par-sum-131072", () => SumN(131072), to);
    Row("par-toarray-64", () => ToArrayN(64), to);
    Row("par-toarray-4096", () => ToArrayN(4096), to);
    Row("nested-seq-body-64-dop8", () => NestedSeq(64, 8), to);
    Row("nested-par-body-64-dop8", () => NestedPar(64, 8), to);
    Row("nested-par-body-64-dop1", () => NestedPar(64, 1), to);
    Row("nested-par-body-4096-dop8", () => NestedPar(4096, 8), to);
    Console.WriteLine("MIN3-OK");
  }
}
