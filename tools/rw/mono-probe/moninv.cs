/* tools/rw/mono-probe/moninv.cs — what System.Threading.Monitor offers in this
 * mscorlib, by arity.  The L50 stall poke calls Enter/PulseAll/Exit through
 * mono_runtime_invoke, so the exact overload arity is a hard requirement. */
using System;
using System.Reflection;
using System.Threading;

class P {
  static void Main() {
    Type t = typeof(Monitor);
    MethodInfo[] ms = t.GetMethods(BindingFlags.Static | BindingFlags.Public |
                                   BindingFlags.NonPublic | BindingFlags.DeclaredOnly);
    for (int i = 0; i < ms.Length; i++)
      Console.WriteLine("MON " + ms[i].Name + "/" + ms[i].GetParameters().Length);
    Console.WriteLine("MONINV-OK");
  }
}
