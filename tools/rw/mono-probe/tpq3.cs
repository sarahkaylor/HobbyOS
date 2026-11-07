/* tpq3.cs — confirm ThreadPoolGlobals.workQueue and produce a NON-EMPTY
 * local queue so the C-side reader's index semantics are checked against
 * managed ground truth. */
using System;
using System.Reflection;
using System.Threading;

class TPQ3
{
    static Assembly c = typeof(ThreadPool).Assembly;
    static FieldInfo F(Type t, string n) { return t.GetField(n, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance); }
    static object G(object o, string n) { return F(o.GetType(), n).GetValue(o); }

    static void Main()
    {
        Type g = c.GetType("System.Threading.ThreadPoolGlobals", false);
        Console.WriteLine("Globals=" + (g != null));
        if (g != null)
            foreach (FieldInfo f in g.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static))
                Console.WriteLine("  G." + f.Name + " : " + f.FieldType.Name + " = " +
                                  (f.IsLiteral ? "(const)" : (f.GetValue(null) == null ? "null" : f.GetValue(null).GetType().Name)));

        Type wpq = c.GetType("System.Threading.ThreadPoolWorkQueue", false);
        Type wsq = wpq.GetNestedType("WorkStealingQueue", BindingFlags.Public | BindingFlags.NonPublic);
        Type sa = wpq.GetNestedType("SparseArray`1", BindingFlags.Public | BindingFlags.NonPublic);

        // Force this thread to be a pool worker: queue one item, and from inside
        // it queue more blocking items, then inspect the worker's own local queue.
        ManualResetEvent outer = new ManualResetEvent(false);
        ManualResetEvent gate = new ManualResetEvent(false);
        ThreadPool.QueueUserWorkItem(delegate
        {
            Console.WriteLine("inside worker tid=" + Thread.CurrentThread.ManagedThreadId);
            Type tl = c.GetType("System.Threading.ThreadPoolWorkQueueThreadLocals", false);
            object locals = F(tl, "threadLocals").GetValue(null);
            Console.WriteLine("threadLocals=" + (locals == null ? "null" : "yes"));
            object q = locals == null ? null : G(locals, "workStealingQueue");
            Console.WriteLine("my wsq=" + (q == null ? "null" : "yes"));
            for (int i = 0; i < 3; i++)
                System.Threading.Tasks.Task.Factory.StartNew(delegate { gate.WaitOne(); });
            // Task scheduling uses preferLocal -> the worker's own deque.
            Dump(q, "after-local-enqueue");
            outer.Set();
            gate.WaitOne();
        });
        outer.WaitOne(3000);
        Thread.Sleep(100);
        object all = F(wpq, "allThreadQueues").GetValue(null);
        Array aa = (Array)G(all, "m_array");
        Console.WriteLine("allThreadQueues n=" + aa.Length);
        for (int i = 0; i < aa.Length; i++)
            if (aa.GetValue(i) != null) Dump(aa.GetValue(i), "slot" + i);
        gate.Set();
        Thread.Sleep(200);
        Console.WriteLine("TPQ3-OK");
    }

    static void Dump(object q, string tag)
    {
        if (q == null) { Console.WriteLine(tag + " null"); return; }
        object arr = G(q, "m_array");
        Array a = (Array)arr;
        int head = (int)G(q, "m_headIndex"), tail = (int)G(q, "m_tailIndex"), mask = (int)G(q, "m_mask");
        Console.Write(tag + " len=" + (a == null ? -1 : a.Length) + " head=" + head + " tail=" + tail + " mask=" + mask + " used=" + (tail - head) + " items=");
        if (a != null) for (int i = head; i < tail; i++) Console.Write((a.GetValue(i & mask) == null ? "null" : "item") + " ");
        Console.WriteLine();
    }
}
