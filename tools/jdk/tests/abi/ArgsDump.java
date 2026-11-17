/** Proves argv survives the exec boundary.  Before the COMPAT_MAX_ARGV fix a
 *  Linux-ABI process silently received at most 16 arguments, which truncated
 *  every realistic JDK tool invocation without any diagnostic. */
public class ArgsDump {
    public static void main(String[] a) {
        System.out.println("ARGSDump count=" + a.length);
        for (int i = 0; i < a.length; i++) System.out.println("ARGSDump [" + i + "]=" + a[i]);
        boolean ok = a.length == 24;
        System.out.println("ARGSDump VERDICT " + (ok ? "PASS all-24-args" : "FAIL expected-24-got-" + a.length));
    }
}
