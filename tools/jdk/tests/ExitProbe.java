import java.util.concurrent.*;

/**
 * ExitProbe -- does the JVM shut down after main() returns, and does the
 * thread count matter?  javac and the 18-check suite both finish their work
 * and then never leave, while a 3-line Hello does; the difference is how many
 * threads the VM has to stop on the way out.
 *
 * Usage: ExitProbe [threads] [halt|exit|return]
 */
public class ExitProbe {
    public static void main(String[] a) throws Exception {
        int n = a.length > 0 ? Integer.parseInt(a[0]) : 8;
        String how = a.length > 1 ? a[1] : "return";
        System.out.println("EXITPROBE start threads=" + n + " how=" + how);

        Thread[] ts = new Thread[n];
        for (int i = 0; i < n; i++) {
            final int id = i;
            ts[i] = new Thread(() -> {
                try { Thread.sleep(20); } catch (InterruptedException e) { }
                if (id % 3 == 0) System.out.println("  worker " + id + " done");
            }, "w" + i);
            ts[i].start();
        }
        for (Thread t : ts) t.join();
        System.out.println("EXITPROBE joined, live threads=" + Thread.activeCount());

        // An executor that IS shut down: the VM must not wait for its pool.
        ExecutorService ex = Executors.newFixedThreadPool(4);
        for (int i = 0; i < 8; i++) ex.submit(() -> { });
        ex.shutdown();
        if (!ex.awaitTermination(30, TimeUnit.SECONDS))
            System.out.println("EXITPROBE pool did not terminate");
        System.out.println("EXITPROBE pool terminated");

        System.out.println("EXITPROBE leaving main via " + how);
        System.out.flush();
        if (how.equals("halt")) Runtime.getRuntime().halt(0);
        if (how.equals("exit")) System.exit(0);
        // "return": falling off the end of main must run shutdown hooks and exit.
    }
}
