/** Isolates the one OpenJDK test that hung inside HobbyOS (String/Split).
 *  Runs its main() on a daemon thread with a long budget so the console says
 *  whether it completes (slow) or never returns (hang), and how long it took. */
public class SplitOnly {
    public static void main(String[] a) throws Exception {
        long t0 = System.nanoTime();
        final Throwable[] err = new Throwable[1];
        Thread th = new Thread(() -> {
            try { Split.main(new String[0]); }
            catch (Throwable e) { err[0] = e; }
        }, "split");
        th.setDaemon(true);
        th.start();
        th.join(240000);
        long ms = (System.nanoTime() - t0) / 1000000L;
        String verdict = th.isAlive() ? "HANG-after-240s"
                       : err[0] != null ? ("FAIL " + err[0]) : "PASS";
        System.out.println("SPLIT-ONLY " + verdict + " elapsed=" + ms + "ms");
    }
}
