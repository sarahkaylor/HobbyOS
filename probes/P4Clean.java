/*
 * probes/P4Clean.java -- is the exec failure a CLEAN failure?
 *
 * The verdict on ProcessBuilder is already "it cannot work", so the remaining
 * question a caller cares about is what the failure costs: does it hang, does
 * it lie, does it leave the JVM alive and its fd table intact, and does the VM
 * still exit on its own afterwards?
 *
 * Each launch is attempted twice, timed, and its message printed verbatim.
 * The fd table is counted through /proc/self/fd (the compat layer's virtual
 * directory) before and after, so a leaked pipe pair shows up as a number
 * rather than an opinion.  main() RETURNS at the end on purpose (no
 * Runtime.halt): the boot must reach "System halt" because the VM shut itself
 * down, not because the probe forced it.
 */
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class P4Clean {
    static void p(String s) { System.out.println("CLEAN: " + s); System.out.flush(); }

    static int fdCount() {
        try (java.util.stream.Stream<Path> s = Files.list(Paths.get("/proc/self/fd"))) {
            return (int) s.count();
        } catch (Throwable t) {
            p("fdCount unavailable: " + t.getClass().getName() + ": " + t.getMessage());
            return -1;
        }
    }

    static void attempt(int n, String label, String... cmd) {
        p("> attempt " + n + " (" + label + ") " + String.join(" ", cmd));
        long t0 = System.nanoTime();
        try {
            ProcessBuilder pb = new ProcessBuilder(cmd);
            Process pr = pb.start();
            p("< started pid=" + pr.pid() + " -- UNEXPECTED");
            pr.destroyForcibly();
        } catch (Throwable t) {
            long ms = (System.nanoTime() - t0) / 1_000_000;
            p("< failed after " + ms + "ms: " + t.getClass().getName());
            p("< message: " + t.getMessage());
            for (Throwable c = t.getCause(); c != null; c = c.getCause())
                p("< cause: " + c.getClass().getName() + ": " + c.getMessage());
            StackTraceElement[] st = t.getStackTrace();
            for (int i = 0; i < Math.min(3, st.length); i++) p("<   at " + st[i]);
        }
    }

    public static void main(String[] args) throws Exception {
        Thread wd = new Thread(() -> {
            try { Thread.sleep(180_000); } catch (InterruptedException e) { return; }
            p("WATCHDOG: still running after 180s");
            Runtime.getRuntime().halt(7);
        }, "wd");
        wd.setDaemon(true);
        wd.start();

        p("start pid=" + ProcessHandle.current().pid()
          + " mech=" + System.getProperty("jdk.lang.Process.launchMechanism", "(default)"));
        int before = fdCount();
        p("fd count before = " + before);

        attempt(1, "native binary", "/LEAF.EXE", "--exit=42");
        attempt(2, "child JVM", "/JDK/bin/java", "-version");
        attempt(3, "native binary again", "/LEAF.EXE");
        attempt(4, "child JVM again", "/JDK/bin/java", "-version");

        int after = fdCount();
        p("fd count after = " + after + " (before=" + before + ", delta="
          + (after < 0 || before < 0 ? "n/a" : String.valueOf(after - before)) + ")");

        /* Runtime.exec is the same native entry point through an older API;
           one call proves the older path is not a separate bug. */
        try {
            Runtime.getRuntime().exec(new String[]{"/LEAF.EXE"});
            p("Runtime.exec UNEXPECTEDLY started a process");
        } catch (Throwable t) {
            p("Runtime.exec failed: " + t.getClass().getName() + ": " + t.getMessage());
        }

        p("still alive after 4 failed launches; main returns now");
        /* No halt(): the JVM must run its exit path on its own. */
    }
}
