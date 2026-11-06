/* tpq.cs — inventory the managed ThreadPoolWorkQueue / WorkStealingQueue in
 * the EXACT mscorlib the game runs.  Reflection only; no assumptions about
 * field names (the point is to learn them, then read them from C in the
 * launcher at the stall).
 *
 * Run under the game's own runtime:
 *   MONO_LIB=<...>/libmonobdwgc-2.0.so MONO_ASSEMBLY_DIR=<...>/Managed \
 *   MONO_CONFIG_DIR=<...>/MonoBleedingEdge/etc ./mono-launch /TPQ.EXE
 */
using System;
using System.Reflection;
using System.Threading;
using System.Collections.Generic;

class TPQ
{
    static Assembly corlib = typeof(ThreadPool).Assembly;

    static void DumpType(string name)
    {
        Type t = corlib.GetType(name, false);
        if (t == null) { Console.WriteLine("TYPE " + name + " = MISSING"); return; }
        Console.WriteLine("TYPE " + t.FullName + " flags=" + t.Attributes);
        FieldInfo[] fs = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic |
                                     BindingFlags.Static | BindingFlags.Instance |
                                     BindingFlags.DeclaredOnly);
        foreach (FieldInfo f in fs)
            Console.WriteLine("  FIELD " + f.Name + " : " + f.FieldType.Name +
                              (f.IsStatic ? " static" : " instance") +
                              (f.IsLiteral ? " const" : ""));
        Type[] ns = t.GetNestedTypes(BindingFlags.Public | BindingFlags.NonPublic);
        foreach (Type n in ns)
            Console.WriteLine("  NESTED " + n.Name + " : " + n.BaseType);
    }

    static object GetStatic(Type t, string f)
    {
        FieldInfo fi = t.GetField(f, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
        return fi == null ? null : fi.GetValue(null);
    }

    static void Main()
    {
        Console.WriteLine("TPQ START runtime=" + Environment.Version + " cpus=" + Environment.ProcessorCount);
        DumpType("System.Threading.ThreadPoolWorkQueue");
        DumpType("System.Threading.ThreadPoolWorkQueueThreadLocals");
        DumpType("System.Threading.WorkStealingQueue");

        Type wpq = corlib.GetType("System.Threading.ThreadPoolWorkQueue", false);
        if (wpq == null) { Console.WriteLine("TPQ-NO-WPQ"); return; }

        // Queue four items that block, so workers exist with local queues.
        ManualResetEvent gate = new ManualResetEvent(false);
        ManualResetEvent started = new ManualResetEvent(false);
        int nstarted = 0;
        for (int i = 0; i < 4; i++)
        {
            ThreadPool.QueueUserWorkItem(delegate
            {
                Interlocked.Increment(ref nstarted);
                started.Set();
                gate.WaitOne();
            });
        }
        started.WaitOne(3000);
        Thread.Sleep(200);
        Console.WriteLine("workers started=" + nstarted);

        // Dump every static field of ThreadPoolWorkQueue with a readable value.
        FieldInfo[] st = wpq.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
        foreach (FieldInfo f in st)
        {
            object v = null;
            try { v = f.GetValue(null); } catch (Exception e) { Console.WriteLine("  STATIC " + f.Name + " <" + e.GetType().Name + ">"); continue; }
            Console.WriteLine("  STATIC " + f.Name + " : " + f.FieldType.Name + " = " + Describe(v));

            if (v is System.Collections.IEnumerable && !(v is string))
            {
                int k = 0;
                foreach (object o in (System.Collections.IEnumerable)v)
                {
                    if (k++ >= 8) { Console.WriteLine("    ..."); break; }
                    Console.WriteLine("    ITEM[" + (k - 1) + "] " + Describe(o));
                    DumpQueue(o);
                }
            }
            else
            {
                DumpQueue(v);
            }
        }

        // Also look at the head/tail chain of the global queue.
        object head = GetStatic(wpq, "queueHead");
        DumpQueue(head);

        gate.Set();
        Thread.Sleep(300);
        Console.WriteLine("TPQ-OK");
    }

    static string Describe(object v)
    {
        if (v == null) return "null";
        try { return v.ToString() + " (" + v.GetType().FullName + ")"; }
        catch { return "<" + v.GetType().FullName + ">"; }
    }

    static void DumpQueue(object q)
    {
        if (q == null) return;
        Type t = q.GetType();
        object arr = null, head = null, tail = null, mask = null;
        try
        {
            FieldInfo fa = t.GetField("m_array", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
            FieldInfo fh = t.GetField("m_headIndex", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
            FieldInfo ft = t.GetField("m_tailIndex", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
            FieldInfo fm = t.GetField("m_mask", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
            if (fa != null) arr = fa.GetValue(q);
            if (fh != null) head = fh.GetValue(q);
            if (ft != null) tail = ft.GetValue(q);
            if (fm != null) mask = fm.GetValue(q);
        }
        catch (Exception e) { Console.WriteLine("    QUEUE-ERR " + e.GetType().Name); }
        Console.WriteLine("    QUEUE " + t.Name + " arr=" + (arr == null ? "null" : Describe(arr)) +
                          " head=" + head + " tail=" + tail + " mask=" + mask +
                          (arr is Array ? " arrlen=" + ((Array)arr).Length : ""));
        // QueueSegment fields too.
        foreach (string fn in new string[] { "nodes", "indexes", "Next", "m_next" })
        {
            FieldInfo f = t.GetField(fn, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
            if (f != null)
            {
                try { Console.WriteLine("      SEG " + fn + " = " + Describe(f.GetValue(q))); }
                catch (Exception e) { Console.WriteLine("      SEG " + fn + " <" + e.GetType().Name + ">"); }
            }
        }
    }
}
