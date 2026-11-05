/* tools/rw/mono-probe/taskinv.cs — inventory probe.
 *
 * Prints, under the GAME's own Mono runtime, the private field layout and
 * method list of the Task machinery, plus the ambient TaskScheduler /
 * SynchronizationContext.  This is the "which field is which" half of the
 * L50 Task introspection: the launcher reads the same classes through
 * mono_class_get_field_from_name, so a name that is absent here is absent
 * there too, and a guess would print nothing.
 */
using System;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;

class P {
  static void Dump(Type t) {
    Console.WriteLine("== " + t.FullName);
    FieldInfo[] fs = t.GetFields(BindingFlags.Instance | BindingFlags.Static |
                                 BindingFlags.Public | BindingFlags.NonPublic |
                                 BindingFlags.DeclaredOnly);
    for (int i = 0; i < fs.Length; i++)
      Console.WriteLine("  F " + fs[i].FieldType.Name + " " + fs[i].Name);
    MethodInfo[] ms = t.GetMethods(BindingFlags.Instance | BindingFlags.Static |
                                   BindingFlags.Public | BindingFlags.NonPublic |
                                   BindingFlags.DeclaredOnly);
    for (int i = 0; i < ms.Length; i++)
      Console.WriteLine("  M " + ms[i].Name + "/" + ms[i].GetParameters().Length +
                        (ms[i].IsVirtual ? " virt" : ""));
  }

  static void Main() {
    Console.WriteLine("RUNTIME=" + Environment.Version + " cpus=" + Environment.ProcessorCount);
    Dump(typeof(Task));
    Dump(typeof(TaskScheduler));
    Dump(typeof(ManualResetEventSlim));
    Dump(typeof(Thread));
    Type cp = typeof(Task).GetNestedType("ContingentProperties",
                                         BindingFlags.NonPublic | BindingFlags.Instance);
    Console.WriteLine("NESTED ContingentProperties=" + (cp == null ? "(null)" : cp.FullName));
    if (cp != null) Dump(cp);
    Type se = typeof(Task).GetNestedType("SetOnInvokeMres",
                                         BindingFlags.NonPublic | BindingFlags.Instance);
    Console.WriteLine("NESTED SetOnInvokeMres=" + (se == null ? "(null)" : se.FullName));
    Type it = typeof(Thread).Assembly.GetType("System.Threading.InternalThread");
    Console.WriteLine("InternalThread=" + (it == null ? "(null)" : it.FullName));
    if (it != null) Dump(it);
    Type th = typeof(Thread).Assembly.GetType("System.Threading.ThreadHandle");
    Console.WriteLine("ThreadHandle=" + (th == null ? "(null)" : th.FullName));
    if (th != null) Dump(th);
    Console.WriteLine("SCHED-CUR=" + TaskScheduler.Current.GetType().FullName);
    Console.WriteLine("SYNCCTX=" + (SynchronizationContext.Current == null ? "(null)" :
                                    SynchronizationContext.Current.GetType().FullName));
    // Inside a pool task, what does Current say?
    Task t = Task.Run(() => {
      Console.WriteLine("SCHED-IN-TASK=" + TaskScheduler.Current.GetType().FullName);
    });
    t.Wait();
    // PLINQ actually working, in the same runtime: the baseline the guest
    // run is compared against.
    int[] src = new int[200000];
    for (int i = 0; i < src.Length; i++) src[i] = i;
    DateTime t0 = DateTime.UtcNow;
    int[] r = System.Linq.Enumerable.ToArray(
        System.Linq.ParallelEnumerable.Select(
            System.Linq.ParallelEnumerable.Where(
                System.Linq.ParallelEnumerable.AsParallel(src), x => (x & 1) == 0), x => x * 3));
    Console.WriteLine("PLINQ n=" + r.Length + " ms=" + (int)(DateTime.UtcNow - t0).TotalMilliseconds +
                      " chk=" + r[12345]);
    Console.WriteLine("TASKINV-OK");
  }
}
