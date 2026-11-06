/* tools/rw/mono-probe/min9.cs — where does the nested-PLINQ 527 ms go?
 *
 * The L52 brief assumes the thread pool never grows past its 8-worker minimum
 * because the hill-climbing monitor thread is never created.  Two facts say
 * the pool *capacity* may not be the cost at all (MONO_THREADS_PER_CPU=8 did
 * not move the 527 ms), so this probe measures the parts separately:
 *
 *   1. pool counters as the managed runtime sees them (GetMin/Max/Available);
 *   2. a plain Task.Run().Wait() from the main thread and from inside a pool
 *      worker -- task round-trip cost with no PLINQ;
 *   3. a 9th task while 8 workers are deliberately blocked -- the hill-climb
 *      growth latency itself (expect ~500 ms if the monitor grows one thread
 *      per MONITOR_INTERVAL tick, ~0 ms once the minimum is 64);
 *   4. the same nested PLINQ query with min=8 and again after
 *      ThreadPool.SetMinThreads(64,64);
 *   5. thread create+join latency (does a pthread_create cost ~500 ms here?).
 *
 * Output is one line per measurement, prefixed MIN9, so a host and a guest log
 * are diffed line by line.
 */
using System;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

class M9 {
  static int[] src = new int[300000];

  static void Counters(string tag) {
    int minW, minIO, maxW, maxIO, avW, avIO;
    ThreadPool.GetMinThreads(out minW, out minIO);
    ThreadPool.GetMaxThreads(out maxW, out maxIO);
    ThreadPool.GetAvailableThreads(out avW, out avIO);
    Console.WriteLine("MIN9 counters " + tag + " min=" + minW + "/" + minIO +
                      " max=" + maxW + "/" + maxIO + " avail=" + avW + "/" + avIO);
  }

  // One nested PLINQ query inside a PLINQ body: the shape that costs 527 ms.
  static void Nested(string tag, int n) {
    int done = 0;
    long t0 = Stopwatch.GetTimestamp();
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
    bool ok = t.Wait(30000);
    long ms = (Stopwatch.GetTimestamp() - t0) * 1000 / Stopwatch.Frequency;
    Console.WriteLine("MIN9 nested " + tag + " done=" + Volatile.Read(ref done) +
                      " ms=" + ms + " per_query_ms=" + (done > 0 ? ms / done : -1) +
                      " ok=" + ok);
  }

  static long TimeTaskRoundTrip(int n) {
    long t0 = Stopwatch.GetTimestamp();
    for (int i = 0; i < n; i++) {
      Task t = Task.Run(() => { });
      if (!t.Wait(10000)) { Console.WriteLine("MIN9 task-roundtrip TIMEOUT at " + i); break; }
    }
    return (Stopwatch.GetTimestamp() - t0) * 1000 / Stopwatch.Frequency;
  }

  // 8 workers blocked, then time a 9th task: the pool must grow or the task waits.
  static void Growth(string tag) {
    ManualResetEventSlim gate = new ManualResetEventSlim(false);
    int entered = 0;
    Task[] blockers = new Task[8];
    for (int i = 0; i < 8; i++) {
      blockers[i] = Task.Factory.StartNew(() => {
        Interlocked.Increment(ref entered);
        gate.Wait(20000);
      }, CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default);
    }
    long t0 = Stopwatch.GetTimestamp();
    while (Volatile.Read(ref entered) < 8) {
      if ((Stopwatch.GetTimestamp() - t0) * 1000 / Stopwatch.Frequency > 5000) break;
      Thread.Yield();
    }
    long armed = (Stopwatch.GetTimestamp() - t0) * 1000 / Stopwatch.Frequency;
    long t1 = Stopwatch.GetTimestamp();
    Task ninth = Task.Run(() => { });
    bool ok = ninth.Wait(10000);
    long ms = (Stopwatch.GetTimestamp() - t1) * 1000 / Stopwatch.Frequency;
    Console.WriteLine("MIN9 growth " + tag + " entered=" + Volatile.Read(ref entered) +
                      " arm_ms=" + armed + " ninth_ms=" + ms + " ok=" + ok);
    gate.Set();
    Task.WaitAll(blockers, 10000);
  }

  static void ThreadCreate(int n) {
    long t0 = Stopwatch.GetTimestamp();
    for (int i = 0; i < n; i++) {
      Thread t = new Thread(() => { });
      t.IsBackground = true;
      t.Start();
      if (!t.Join(10000)) { Console.WriteLine("MIN9 thread-create TIMEOUT at " + i); break; }
    }
    long ms = (Stopwatch.GetTimestamp() - t0) * 1000 / Stopwatch.Frequency;
    Console.WriteLine("MIN9 thread-create n=" + n + " ms=" + ms + " per=" + (n > 0 ? ms / n : -1));
  }

  static void Main(string[] argv) {
    for (int i = 0; i < src.Length; i++) src[i] = i;
    Console.WriteLine("MIN9 start cpus=" + Environment.ProcessorCount +
                      " tp_per_cpu=" + (Environment.GetEnvironmentVariable("MONO_THREADS_PER_CPU") ?? "-"));
    Counters("start");
    ThreadCreate(20);

    long mainTrip = TimeTaskRoundTrip(50);
    Console.WriteLine("MIN9 task-roundtrip main n=50 ms=" + mainTrip + " per=" + (mainTrip / 50));

    Nested("min-default", 3);
    Counters("after-nested-default");

    Growth("min-default");
    Counters("after-growth-default");

    int minW, minIO;
    ThreadPool.GetMinThreads(out minW, out minIO);
    bool set = ThreadPool.SetMinThreads(64, minIO < 64 ? 64 : minIO);
    Console.WriteLine("MIN9 setmin 64 rc=" + set);
    Counters("after-setmin");

    Nested("min-64", 3);
    Growth("min-64");
    Counters("end");

    long workerTrip = 0;
    Task rt = Task.Factory.StartNew(() => {
      workerTrip = TimeTaskRoundTrip(20);
    }, CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default);
    rt.Wait(60000);
    Console.WriteLine("MIN9 task-roundtrip worker n=20 ms=" + workerTrip + " per=" + (workerTrip / 20));
    Console.WriteLine("MIN9-OK");
  }
}
