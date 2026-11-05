// tp_starve.cs — M6/L44: RimWorld's nested-parallelism shape.
//
// GenThreading.ParallelForEach (Verse/GenThreading.cs) queues one ThreadPool
// work item per batch and then BLOCKS the calling thread on an
// AutoResetEvent.WaitOne() until every batch reported done.  PlayDataLoader
// reaches it from inside Mono's Parallel.ForEach, so the outer iterations are
// themselves ThreadPool workers: when N outer workers each queue inner items
// and block, the inner items sit in the workers' local work-stealing queues.
//
// This probe reproduces that shape and prints one row per stage, so a run that
// stops after "outer queued" names a ThreadPool that never grows.  Host and
// guest run the same bytes under the game's runtime.
using System;
using System.Threading;

static class TPStarve
{
    static void Outer(object state)
    {
        AutoResetEvent done = (AutoResetEvent)state;
        const int total = 16;
        int finished = 0;
        for (int i = 0; i < total; i++)
        {
            ThreadPool.QueueUserWorkItem(delegate
            {
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
                return;
            }
        }
        Console.WriteLine("STARVE outer done spins=" + spins);
    }

    static int Main()
    {
        int outer = Environment.ProcessorCount;
        Console.WriteLine("STARVE START cpus=" + outer);
        AutoResetEvent[] evs = new AutoResetEvent[outer];
        for (int i = 0; i < outer; i++)
        {
            evs[i] = new AutoResetEvent(false);
            ThreadPool.QueueUserWorkItem(Outer, evs[i]);
        }
        Console.WriteLine("STARVE outer queued");
        for (int i = 0; i < outer; i++)
            evs[i].WaitOne();
        Console.WriteLine("STARVE outer all done");
        Console.WriteLine("STARVE-OK");
        return 0;
    }
}
