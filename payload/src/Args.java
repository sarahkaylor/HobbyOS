/**
 * Prints the argv the guest actually delivered, so a tool that behaves as if a
 * flag was missing can be traced to the argument vector instead of guessed at.
 */
public class Args {
    public static void main(String[] a) {
        System.out.println("[ARGS] argc=" + a.length);
        for (int i = 0; i < a.length; i++) {
            System.out.println("[ARGS] argv[" + i + "]=\"" + a[i] + "\" (" + a[i].length() + ")");
        }
        System.out.flush();
    }
}
