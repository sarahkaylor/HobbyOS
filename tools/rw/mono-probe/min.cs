/* tools/rw/mono-probe/min.cs — minimise the guest-only PLINQ hang.
 *
 * The game stalls in
 *   Monitor.Wait <- ManualResetEventSlim.Wait <- Task.SpinThenBlockingWait
 *   <- Task.InternalRunSynchronously <- Task.RunSynchronously
 *   <- SpoolingTask.SpoolStopAndGo
 * with the awaited Task COMPLETE (`RAN_TO_COMPLETION`, action NULL), its
 * completion SetOnInvokeMres SET (bit 31 of m_combinedState) and one waiter
 * still registered (bits 0..18 == 1), frozen for 130 s.
 *
 * Each shape prints START and OK, so a hang names itself.  Shapes are ordered
 * cheapest-first; every one is a candidate minimal reproduction of the same
 * "completion signalled, waiter still asleep" state.
 */
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

class M {
  static int N = 200;

  static void Shape(string name, Action a) {
    Console.WriteLine("SHAPE " + name + " start");
    int t0 = Environment.TickCount;
    a();
    Console.WriteLine("SHAPE " + name + " OK ms=" + (Environment.TickCount - t0));
  }

  // 1. The game's own shape, one line: RunSynchronously on a task that has an
  //    attached child.  The parent parks in WAITING_ON_CHILDREN; the child
  //    completes on a pool thread, which runs the parent's continuations
  //    (FinishStageThree -> SetOnInvokeMres.Set) there, while the parent is
  //    inside SpinThenBlockingWait's ManualResetEventSlim.Wait.
  static void RunSyncAttachedChild(int n) {
    for (int i = 0; i < n; i++) {
      Task t = new Task(delegate {
        Task.Factory.StartNew(delegate { Thread.Sleep(1); },
                              CancellationToken.None,
                              TaskCreationOptions.AttachedToParent,
                              TaskScheduler.Default);
      });
      t.RunSynchronously();
      if (!t.IsCompleted) throw new Exception("not complete " + i);
    }
  }

  // 2. MRES storm: a pool thread Sets while the caller Waits, with no
  //    synchronisation at all, so Set lands before / during / after Wait.
  //    ManualResetEventSlim is sticky, so no iteration may ever block.
  static void MresStorm(int n) {
    ManualResetEventSlim e = new ManualResetEventSlim(false);
    for (int i = 0; i < n; i++) {
      e.Reset();
      int go = 0;
      Task t = Task.Factory.StartNew(delegate {
        while (Volatile.Read(ref go) == 0) Thread.SpinWait(20);
        e.Set();
      }, CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default);
      Volatile.Write(ref go, 1);
      if (!e.Wait(3000)) throw new Exception("MRES HANG i=" + i);
      t.Wait(3000);
    }
  }

  // 3. Same storm with a dedicated (non-pool) setter thread.
  static void MresStormThread(int n) {
    ManualResetEventSlim e = new ManualResetEventSlim(false);
    ManualResetEventSlim go = new ManualResetEventSlim(false);
    bool stop = false;
    Thread s = new Thread(() => {
      while (true) {
        go.Wait();
        if (Volatile.Read(ref stop)) return;
        e.Set();
        go.Reset();
      }
    });
    s.IsBackground = true;
    s.Start();
    for (int i = 0; i < n; i++) {
      e.Reset();
      go.Set();
      if (!e.Wait(3000)) throw new Exception("MRES-T HANG i=" + i);
    }
    Volatile.Write(ref stop, true);
    go.Set();
  }

  // 4. Ordered Monitor.Wait / PulseAll: the pulser cannot run before the
  //    waiter is queued, so a lost pulse here is a monitor defect.
  static void MonitorOrdered(int n) {
    object o = new object();
    int ready = 0, phase = 0;
    Thread w = new Thread(() => {
      for (int i = 0; i < n; i++) {
        lock (o) {
          Volatile.Write(ref ready, i + 1);
          while (Volatile.Read(ref phase) <= i) Monitor.Wait(o);
        }
      }
    });
    w.IsBackground = true;
    w.Start();
    for (int i = 0; i < n; i++) {
      while (Volatile.Read(ref ready) <= i) Thread.Yield();
      lock (o) { Volatile.Write(ref phase, i + 1); Monitor.PulseAll(o); }
    }
    if (!w.Join(20000)) throw new Exception("MONITOR HANG at phase=" + Volatile.Read(ref phase));
  }

  // 5. A task that waits on its own child through Task.Wait (the public path).
  static void TaskWaitChild(int n) {
    for (int i = 0; i < n; i++) {
      Task child = Task.Factory.StartNew(delegate { Thread.Sleep(1); });
      Task parent = Task.Factory.StartNew(delegate { child.Wait(); });
      if (!parent.Wait(5000)) throw new Exception("TASK-WAIT HANG i=" + i);
    }
  }

  // 6. Plain thread join storm (no managed monitor involved).
  static void JoinStorm(int n) {
    for (int i = 0; i < n; i++) {
      Thread t = new Thread(() => { Thread.Sleep(0); });
      t.Start();
      if (!t.Join(5000)) throw new Exception("JOIN HANG i=" + i);
    }
  }

  static void Main(string[] argv) {
    if (argv.Length > 0) N = int.Parse(argv[0]);
    Console.WriteLine("MIN-PROBE start cpus=" + Environment.ProcessorCount + " n=" + N);
    Shape("join-storm", () => JoinStorm(N));
    Shape("mres-storm", () => MresStorm(N));
    Shape("mres-storm-thread", () => MresStormThread(N));
    Shape("monitor-ordered", () => MonitorOrdered(N));
    Shape("task-wait-child", () => TaskWaitChild(N));
    Shape("runsync-attached-child", () => RunSyncAttachedChild(N));
    Console.WriteLine("MIN-OK");
  }
}
