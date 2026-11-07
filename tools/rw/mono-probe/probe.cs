/* probe.cs — M6/L31: the managed-layer question, asked directly.
 *
 * The engine (guest only) logs "RuntimeInitializeOnLoadManagerInitializer:
 * Failed reading 'ScriptingAssemblies.json'" and then cannot resolve a single
 * type out of UnityEngine.CoreModule.dll, while the same build on the host has
 * no such row and loads every assembly.  This probe runs under the guest's own
 * Mono 6.12 and asks the questions in order:
 *
 *   1. can managed code read a FAT file at all (File.ReadAllText)?
 *   2. can managed code read a file over NFS (File.ReadAllBytes)?
 *   3. can Mono parse that file as an assembly and enumerate its types?
 *
 * A NULL for UnityEngine.Playables.Playable here means Mono's image is empty
 * even though the bytes are on disk; a correct type list means the failure is
 * above Mono (Unity's own image handle).  A TypeInitializationException for
 * 'Sys' means Mono's libc shim (libmono-native.so via the System.Native
 * dllmap) never came up — which breaks EVERY managed file operation.
 *
 * Build (csc from the dotnet SDK, against Mono's own mscorlib):
 *   dotnet <sdk>/Roslyn/bincore/csc.dll -nostdlib \
 *     -r:third_party/rw-mono/root/usr/lib/mono/4.5/mscorlib.dll \
 *     -target:exe -out:probe.exe probe.cs
 */
using System;
using System.IO;
using System.Reflection;

class Probe
{
    static void Dump(Exception e, string what)
    {
        Console.WriteLine("PROBE " + what + " EX " + e.GetType().FullName + ": " + e.Message);
        Exception inr = e.InnerException;
        int depth = 0;
        while (inr != null && depth < 4)
        {
            Console.WriteLine("PROBE   inner[" + depth + "] " + inr.GetType().FullName + ": " + inr.Message);
            inr = inr.InnerException;
            depth++;
        }
    }

    static void Text(string p)
    {
        try
        {
            string s = File.ReadAllText(p);
            Console.WriteLine("PROBE text " + p + " len=" + s.Length);
        }
        catch (Exception e) { Dump(e, "text " + p); }
    }

    static void One(string dir, string file)
    {
        string p = Path.Combine(dir, file);
        byte[] b;
        try
        {
            b = File.ReadAllBytes(p);
        }
        catch (Exception e)
        {
            Dump(e, "read " + file);
            return;
        }
        Console.WriteLine("PROBE read " + file + " bytes=" + b.Length
                          + " mz=" + b[0].ToString("x2") + b[1].ToString("x2"));

        try
        {
            Assembly a = Assembly.LoadFrom(p);
            Console.WriteLine("PROBE asm " + a.FullName);
            Type t = a.GetType("UnityEngine.Playables.Playable");
            Console.WriteLine("PROBE type Playable=" + (t == null ? "NULL" : t.FullName));
            Type t2 = a.GetType("UnityEngine.PlayerLoop.PostLateUpdate");
            Console.WriteLine("PROBE type PostLateUpdate=" + (t2 == null ? "NULL" : t2.FullName));
            Type[] all = a.GetTypes();
            Console.WriteLine("PROBE types=" + all.Length);
        }
        catch (Exception e) { Dump(e, "asm " + file); }
    }

    static void Main()
    {
        Console.WriteLine("PROBE start runtime=" + Environment.Version
                          + " config=" + AppDomain.CurrentDomain.SetupInformation.ConfigurationFile);
        Text("/WAVE.CFG");
        One("/RWMANAGED", "UnityEngine.CoreModule.dll");
        One("/RWMANAGED", "Assembly-CSharp.dll");
        Console.WriteLine("PROBE done");
    }
}
