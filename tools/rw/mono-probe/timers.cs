/* timers.cs — L49: does the managed runtime's OWN timer machinery work here?
 *
 * Every case is a shape the loader could be blocked in:
 *   A  Task.Delay(50).ContinueWith(...) + Wait      (timer -> continuation)
 *   B  new Timer(cb, null, 50, Infinite) + WaitOne  (System.Threading.Timer)
 *   C  Task.Run(() => 42).Wait(2000)                (pool item -> completion)
 *   D  Task.Factory.StartNew(...).ContinueWith(...) (chained continuations)
 *   E  Task.Delay(50).Wait()                        (Task.Wait on a timer task)
 *
 * A case that hangs is reported as "TIMERS TIMEOUT phase=X" from a watchdog
 * thread, so a guest run always ends with a row instead of a silent stall --
 * the shape that cost this track three lanes.
 */
using System;
using System.Threading;
using System.Threading.Tasks;

class Timers
{
    static int phase;
    static readonly ManualResetEvent done = new ManualResetEvent(false);

    static void Watchdog()
    {
        for (int i = 0; i < 120; i++)
        {
            Thread.Sleep(100);
            if (done.WaitOne(0)) return;
        }
        Console.WriteLine("TIMERS TIMEOUT phase=" + phase);
        Console.Out.Flush();
        Environment.Exit(3);
    }

    static void Main()
    {
        new Thread(Watchdog) { IsBackground = true }.Start();

        phase = 1;
        var t1 = Task.Delay(50).ContinueWith(_ => 1);
        if (!t1.Wait(5000) || t1.Result != 1) { Console.WriteLine("A FAIL"); Environment.Exit(1); }
        Console.WriteLine("A Task.Delay(50).ContinueWith OK"); Console.Out.Flush();

        phase = 2;
        var ev = new ManualResetEvent(false);
        var timer = new Timer(_ => ev.Set(), null, 50, Timeout.Infinite);
        if (!ev.WaitOne(5000)) { Console.WriteLine("B FAIL"); Environment.Exit(1); }
        timer.Dispose();
        Console.WriteLine("B Timer(50ms) OK"); Console.Out.Flush();

        phase = 3;
        var t3 = Task.Run(() => 42);
        if (!t3.Wait(5000) || t3.Result != 42) { Console.WriteLine("C FAIL"); Environment.Exit(1); }
        Console.WriteLine("C Task.Run().Wait OK"); Console.Out.Flush();

        phase = 4;
        var t4 = Task.Factory.StartNew(() => 7).ContinueWith(x => x.Result * 2);
        if (!t4.Wait(5000) || t4.Result != 14) { Console.WriteLine("D FAIL"); Environment.Exit(1); }
        Console.WriteLine("D chained continuations OK"); Console.Out.Flush();

        phase = 5;
        Task.Delay(50).Wait();
        Console.WriteLine("E Task.Delay(50).Wait OK"); Console.Out.Flush();

        phase = 6;
        long t0 = DateTime.UtcNow.Ticks;
        Task.Delay(300).Wait();
        long ms = (DateTime.UtcNow.Ticks - t0) / TimeSpan.TicksPerMillisecond;
        Console.WriteLine("F Task.Delay(300).Wait ms=" + ms);
        if (ms < 250) { Console.WriteLine("F FAIL short"); Environment.Exit(1); }

        done.Set();
        Console.WriteLine("TIMERS-OK");
        Console.Out.Flush();
        Environment.Exit(0);
    }
}
