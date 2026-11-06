/* tools/rw/mono-probe/min8.cs — does the MemoryBarrier contract that
 * ManualResetEventSlim depends on actually hold in this process?
 *
 * ManualResetEventSlim.Wait relies on a specific claim (Mono's own source,
 * ManualResetEventSlim.cs:649-663):
 *
 *   "There is a ---- that Set will fail to see that there are waiters as Set
 *    does not take the lock, so after updating waiters, we must check IsSet
 *    again.  Also, we must ensure there cannot be any reordering of the
 *    assignment to Waiters and the read from IsSet.  This is guaranteed as
 *    Waiters{set;} involves an Interlocked.CompareExchange operation which
 *    provides a full memory barrier."
 *
 * If that guarantee is violated, Set() reads Waiters == 0, skips
 * `lock (m_lock) { Monitor.PulseAll(...) }`, and the waiter -- which already
 * saw IsSet == false -- parks for ever on an event nobody will ever set.
 * `m_combinedState` would then freeze at set=1, waiters=1: exactly the state
 * docs/rimworld-launch-log.md 37.2 measured in the game.
 *
 * This probe runs that handshake directly, two threads at a time, and counts
 * the windows where BOTH sides saw the old value.  Any hit is a lost wake.
 */
using System;
using System.Threading;
using System.Threading.Tasks;

class M8 {
  const int SET = unchecked((int)0x80000000);
  static int state;
  static int turn;            // main bumps this to release a round
  static int seqA, seqB;      // each worker publishes the round it finished
  static int ra, rb;          // what each side observed
  static int casA, casB;      // proof the CAS actually ran

  static void Main(string[] argv) {
    int n = argv.Length > 0 ? int.Parse(argv[0]) : 1000000;
    Console.WriteLine("MIN8 start cpus=" + Environment.ProcessorCount + " n=" + n);

    Thread A = new Thread(() => {
      for (int i = 0; i < n; i++) {
        while (Volatile.Read(ref turn) < i + 1) Thread.SpinWait(4);
        // waiter: increment Waiters (CAS retry, full barrier), then read IsSet
        int old = Volatile.Read(ref state);
        while (Interlocked.CompareExchange(ref state, old | 1, old) != old)
          old = Volatile.Read(ref state);
        casA++;
        ra = (Volatile.Read(ref state) & SET) != 0 ? 1 : 0;
        Volatile.Write(ref seqA, i + 1);
      }
    });
    Thread B = new Thread(() => {
      for (int i = 0; i < n; i++) {
        while (Volatile.Read(ref turn) < i + 1) Thread.SpinWait(4);
        // setter: set the signalled bit (CAS retry, full barrier), then read Waiters
        int old = Volatile.Read(ref state);
        while (Interlocked.CompareExchange(ref state, old | SET, old) != old)
          old = Volatile.Read(ref state);
        casB++;
        rb = (Volatile.Read(ref state) & 0x7ffff) != 0 ? 1 : 0;
        Volatile.Write(ref seqB, i + 1);
      }
    });
    A.IsBackground = true; B.IsBackground = true;
    A.Start(); B.Start();

    int lost = 0, iters = 0;
    int t0 = Environment.TickCount;
    for (int i = 0; i < n; i++) {
      state = 0; ra = rb = -1;
      Volatile.Write(ref turn, i + 1);
      while (Volatile.Read(ref seqA) < i + 1 || Volatile.Read(ref seqB) < i + 1) {
        if (Environment.TickCount - t0 > 120000) {
          Console.WriteLine("MIN8 TIMEOUT iters=" + iters + " lost=" + lost +
                            " seqA=" + seqA + " seqB=" + seqB + " want=" + (i + 1));
          return;
        }
        Thread.SpinWait(8);
      }
      iters++;
      // both saw "the other has not acted yet" -> the pulse would be dropped
      if (ra == 0 && rb == 0) lost++;
    }
    Console.WriteLine("MIN8 iters=" + iters + " lost_windows=" + lost +
                      " casA=" + casA + " casB=" + casB +
                      " ms=" + (Environment.TickCount - t0));
    if (lost > 0) Console.WriteLine("MIN8 DEKKER-VIOLATED");
    else Console.WriteLine("MIN8 DEKKER-OK");
  }
}
