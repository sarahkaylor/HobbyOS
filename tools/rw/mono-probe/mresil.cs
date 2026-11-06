/* mresil.cs — decode ManualResetEventSlim.Wait/Set IL from the game's own
 * mscorlib, using the runtime's OpCodes table (correct operand widths). */
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Threading;

class MresIL
{
    static Module mod;
    static Dictionary<short, OpCode> ops = new Dictionary<short, OpCode>();
    static readonly Dictionary<short, int> widths = new Dictionary<short, int>();

    static void Main()
    {
        foreach (FieldInfo f in typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static))
        {
            OpCode o = (OpCode)f.GetValue(null);
            ops[o.Value] = o;
        }
        Type t = typeof(ManualResetEventSlim);
        mod = t.Module;
        foreach (MethodInfo m in t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly))
        {
            if (m.Name != "Wait" && m.Name != "Set" && m.Name != "IsSet" && m.Name != "TrySet"
                && m.Name != "get_IsSet" && m.Name != "get_Waiters" && m.Name != "set_Waiters") continue;
            ParameterInfo[] ps = m.GetParameters();
            string sig = m.Name + "(";
            for (int i = 0; i < ps.Length; i++) sig += (i > 0 ? "," : "") + ps[i].ParameterType.Name;
            Console.WriteLine("=== " + sig + ")");
            MethodBody b = m.GetMethodBody();
            if (b == null) { Console.WriteLine("  (no body)"); continue; }
            byte[] il = b.GetILAsByteArray();
            int p = 0;
            while (p < il.Length)
            {
                int at = p;
                short code = il[p++];
                if (code == 0xFE) code = (short)(0xFE00 | il[p++]);
                OpCode o;
                if (!ops.TryGetValue(code, out o)) { Console.WriteLine("  ??" + code.ToString("x") + " at " + at.ToString("x")); break; }
                object operand = null;
                switch (o.OperandType)
                {
                    case OperandType.InlineNone: break;
                    case OperandType.ShortInlineI:
                        operand = (sbyte)il[p]; p += 1; break;
                    case OperandType.ShortInlineVar:
                        operand = (int)il[p]; p += 1; break;
                    case OperandType.InlineVar:
                        operand = (int)BitConverter.ToUInt16(il, p); p += 2; break;
                    case OperandType.InlineI:
                        operand = BitConverter.ToInt32(il, p); p += 4; break;
                    case OperandType.InlineI8:
                        operand = BitConverter.ToInt64(il, p); p += 8; break;
                    case OperandType.ShortInlineR:
                        operand = BitConverter.ToSingle(il, p); p += 4; break;
                    case OperandType.InlineR:
                        operand = BitConverter.ToDouble(il, p); p += 8; break;
                    case OperandType.InlineString:
                        { int tok = BitConverter.ToInt32(il, p); p += 4; operand = "str:" + tok.ToString("x"); break; }
                    case OperandType.InlineSwitch:
                        {
                            int n = BitConverter.ToInt32(il, p); p += 4;
                            int[] tg = new int[n];
                            for (int i = 0; i < n; i++) { tg[i] = BitConverter.ToInt32(il, p); p += 4; }
                            operand = n + " targets";
                            break;
                        }
                    case OperandType.InlineBrTarget:
                        operand = BitConverter.ToInt32(il, p); p += 4; break;
                    case OperandType.ShortInlineBrTarget:
                        operand = (sbyte)il[p]; p += 1; break;
                    case OperandType.InlineMethod:
                    case OperandType.InlineField:
                    case OperandType.InlineType:
                    case OperandType.InlineTok:
                        {
                            int tok = BitConverter.ToInt32(il, p); p += 4;
                            try
                            {
                                if (o.OperandType == OperandType.InlineMethod) operand = mod.ResolveMethod(tok).ToString();
                                else if (o.OperandType == OperandType.InlineField) operand = mod.ResolveField(tok).ToString();
                                else operand = mod.ResolveType(tok).ToString();
                            }
                            catch { operand = "tok:" + tok.ToString("x"); }
                            break;
                        }
                    default: operand = "?"; break;
                }
                string show = o.Name;
                if (operand != null) show += " " + operand;
                Console.WriteLine("  " + at.ToString("x4") + ": " + show);
            }
        }
    }
}
