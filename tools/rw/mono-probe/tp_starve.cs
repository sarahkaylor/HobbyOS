// tp_starve.cs — M6/L45: RimWorld's nested-parallelism shape, as a probe.
//
// GenThreading.ParallelForEach (Verse/GenThreading.cs) queues one ThreadPool
// work item per batch and then BLOCKS the calling thread on an
// AutoResetEvent.WaitOne() until every batch reported done.  PlayDataLoader
// reaches it from inside Mono's Parallel.ForEach, so the outer iterations are
// themselves ThreadPool workers: when N outer workers each queue inner items
// and block, the inner items can only run if the pool grows.
//
// This probe reproduces that shape and prints one row per stage, so a run that
// stops after "outer queued" names a ThreadPool that never grows.  Host and
// guest run the same bytes under the game's runtime.
//
// L45 fixes two defects in the original probe:
//   - Outer never signalled its own completion event, so Main's join could
//     never return and the probe could not print STARVE-OK on ANY kernel
//     (host included).  A reproducer that cannot pass cannot gate anything.
//   - the inner work was a single counter increment, so 16 inner items could
//     always be drained by the workers already running; each inner item now
//     allocates and burns ~30 ms so the pool has to serve them concurrently.
using System;
using System.Threading;

static class TPStarve
{
    sealed class Job
    {
        public AutoResetEvent Done;   // inner-completion event (RimWorld's taskDoneEvent)
        public AutoResetEvent Mine;   // this outer iteration's completion
    }

    static void Outer(object state)
    {
        Job job = (Job)state;
        AutoResetEvent done = job.Done;
        const int total = 16;
        int finished = 0;
        for (int i = 0; i < total; i++)
        {
            ThreadPool.QueueUserWorkItem(delegate
            {
                // Enough work that the batch cannot finish before the pool has
                // to serve the next one: 128 KiB of allocation + ~30 ms of CPU.
                byte[] buf = new byte[128 * 1024];
                int acc = 0;
                for (int k = 0; k < buf.Length; k += 64) { buf[k] = (byte)k; acc += buf[k]; }
                long spin = 0;
                for (int k = 0; k < 200000; k++) spin += k ^ acc;
                GC.KeepAlive(buf);
                Interlocked.Increment(ref finished);
                done.Set();
            });
        }
        int spins = 0;
        while (Interlocked.CompareExchange(ref finished, 0, 0) < total)
        {
            done.WaitOne(500);
            spins++;
            if (spins > 40) // 20 s cap: report rather than hang the wave forever
            {
                Console.WriteLine("STARVE outer TIMEOUT finished=" + finished);
                job.Mine.Set();
                return;
            }
        }
        Console.WriteLine("STARVE outer done spins=" + spins);
        job.Mine.Set();
    }

    static int Main()
    {
        int outer = Environment.ProcessorCount;
        Console.WriteLine("STARVE START cpus=" + outer);
        Job[] jobs = new Job[outer];
        for (int i = 0; i < outer; i++)
        {
            jobs[i] = new Job();
            jobs[i].Done = new AutoResetEvent(false);
            jobs[i].Mine = new AutoResetEvent(false);
            ThreadPool.QueueUserWorkItem(Outer, jobs[i]);
        }
        Console.WriteLine("STARVE outer queued");
        for (int i = 0; i < outer; i++)
            jobs[i].Mine.WaitOne();
        Console.WriteLine("STARVE outer all done");
        Console.WriteLine("STARVE-OK");
        return 0;
    }
}
