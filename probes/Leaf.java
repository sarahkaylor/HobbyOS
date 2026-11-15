/*
 * probes/Leaf.java -- the program a parent is supposed to be able to run.
 *
 * Deliberately boring and independent of every parent mechanism: it does not
 * use ProcessBuilder, does not fork, and does not touch the JDK's launcher.
 * If Leaf cannot be started by the KERNEL (as the wave program) then every
 * later failure is "the leaf never runs", not "exec is broken".  Run it both
 * ways and compare.
 *
 * Usage: java Leaf [args...]
 *   --exit=N     exit with N (default 0)
 *   --echo       copy stdin to stdout until EOF (pipe test)
 *   --sleep=MS   stay alive (the SIGTERM/destroy target)
 *   --stderr     also write one line to stderr
 */
import java.io.*;

public class Leaf {
    static void out(String s) { System.out.println("LEAF: " + s); System.out.flush(); }

    public static void main(String[] argv) throws Exception {
        int code = 0, sleepMs = 0;
        boolean echo = false, err = false;
        for (String a : argv) {
            if (a.startsWith("--exit=")) code = Integer.parseInt(a.substring(7));
            else if (a.equals("--echo")) echo = true;
            else if (a.startsWith("--sleep=")) sleepMs = Integer.parseInt(a.substring(8));
            else if (a.equals("--stderr")) err = true;
        }
        out("start pid=" + ProcessHandle.current().pid() + " argc=" + argv.length);
        StringBuilder sb = new StringBuilder();
        for (String a : argv) { if (sb.length() > 0) sb.append('|'); sb.append(a); }
        out("argv=" + sb);
        out("cwd=" + new File(".").getAbsolutePath());
        out("env.PROBE=" + System.getenv("PROBE") + " env.size=" + System.getenv().size());
        if (err) System.err.println("LEAF: this line went to stderr");
        if (echo) {
            BufferedReader r = new BufferedReader(new InputStreamReader(System.in, "UTF-8"));
            String line;
            int n = 0;
            while ((line = r.readLine()) != null) {
                out("echo[" + n++ + "]=" + line);
            }
            out("echo eof lines=" + n);
        }
        if (sleepMs > 0) {
            out("sleeping " + sleepMs + "ms");
            Thread.sleep(sleepMs);
        }
        out("exiting code=" + code);
        System.exit(code);
    }
}
