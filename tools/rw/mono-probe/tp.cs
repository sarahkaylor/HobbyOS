/* tp.cs — L42/L43: the exact managed wait shape RimWorld's loader uses.
 *
 * Verse.GenThreading.ParallelFor/ParallelForEach create an AutoResetEvent,
 * queue one ThreadPool work item per batch, and each item does
 * Interlocked.Add(tasksDone) then taskDoneEvent.Set(); the caller loops
 * `while (tasksDone < count) taskDoneEvent.WaitOne();`.
 *
 * RUN IT UNDER THE GAME'S OWN RUNTIME, NOT mono-sgen.  The official 6.12
 * mono-sgen is monolithic and hides four mono_* symbols that
 * libmono-native.so imports, so System.Native cannot load and EVERY threadpool
 * worker dies in ThreadPoolWorkQueueThreadLocals..ctor -> new Random() ->
 * Interop.Sys -- on the HOST too, printing "TP TIMEOUT parallel done=0" for a
 * reason that has nothing to do with this kernel.  tools/rw/run-tp-probe.sh
 * drives MonoBleedingEdge/libmonobdwgc-2.0.so through mono_main instead (the
 * exact runtime the game runs) and tools/gate/m2_tp.sh grades it.
 *
 * A case that never returns prints TP TIMEOUT with the counter it reached.
 */
using System;
using System.Threading;

class TP
{
    const int N = 8;
    static int done;

    static void CaseParallelFor()
    {
        done = 0;
        AutoResetEvent ev = new AutoResetEvent(false);
        for (int i = 0; i < N; i++)
        {
            int k = i;
            ThreadPool.QueueUserWorkItem(delegate
            {
                Thread.SpinWait(2000 + k * 500);
                Interlocked.Increment(ref done);
                ev.Set();
            });
        }
        int spins = 0;
        while (Interlocked.CompareExchange(ref done, 0, 0) < N)
        {
            if (!ev.WaitOne(5000))
            {
                Console.WriteLine("TP TIMEOUT parallel done=" + done);
                return;
            }
            if (++spins > 100000)
            {
                Console.WriteLine("TP TIMEOUT spin done=" + done);
                return;
            }
        }
        Console.WriteLine("TP OK parallel done=" + done);
    }

    static void CaseJoin()
    {
        int v = 0;
        Thread t = new Thread(new ThreadStart(delegate { v = 7; }));
        t.Start();
        t.Join();
        Console.WriteLine("TP OK join v=" + v);
    }

    static void Main()
    {
        Console.WriteLine("TP START cpus=" + Environment.ProcessorCount);
        Console.WriteLine("TP phase before-join");
        CaseParallelFor();
        CaseJoin();
        Console.WriteLine("TP phase after-join");
        CaseParallelFor();
        for (int r = 0; r < 5; r++)
            CaseParallelFor();
        Console.WriteLine("TP-OK");
    }
}
