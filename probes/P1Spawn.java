/*
 * probes/P1Spawn.java -- step 1 of the exec investigation: can a JVM start
 * ANY second program at all, and if not, exactly which call says no?
 *
 * Every step is bracketed by a "PROBE: > step" line before the call and a
 * "PROBE: < step" line after it, so a guest that dies or wedges inside the
 * call still leaves the last-reached step on the console.  That is the whole
 * point: the log is the stack trace when the JVM cannot produce one.
 *
 * The launch mechanism is printed because the JDK has three (POSIX_SPAWN,
 * VFORK, FORK) and they take completely different kernel paths:
 *   POSIX_SPAWN -> glibc clone(CLONE_VM|CLONE_VFORK|SIGCHLD)
 *   FORK/VFORK  -> fork(2) then execve(/JDK/lib/jspawnhelper)
 * "error=38" on one and a wedge on the other is not the same bug.
 */
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public class P1Spawn {
    static void p(String s) { System.out.println("PROBE: " + s); System.out.flush(); }
    static void bad(String s) { System.out.println("PROBE-FAIL: " + s); System.out.flush(); }

    static Thread watchdog(final int ms) {
        Thread t = new Thread(() -> {
            try { Thread.sleep(ms); } catch (InterruptedException e) { return; }
            p("WATCHDOG fired after " + ms + "ms -- the step above never returned");
            Runtime.getRuntime().halt(7);
        }, "watchdog");
        t.setDaemon(true);
        t.start();
        return t;
    }

    /* Drain a stream on a worker so a full pipe cannot deadlock the parent
       (which is itself a pipe test: 64 KiB in, 64 KiB out). */
    static Future<String> drain(final InputStream in, final String tag) {
        return Executors.newSingleThreadExecutor(r -> {
            Thread t = new Thread(r, "drain-" + tag);
            t.setDaemon(true);
            return t;
        }).submit(() -> {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] b = new byte[4096];
            int n;
            while ((n = in.read(b)) > 0) bos.write(b, 0, n);
            return new String(bos.toByteArray(), StandardCharsets.UTF_8);
        });
    }

    static void show(String what, Throwable t) {
        bad(what + " -> " + t.getClass().getName() + ": " + t.getMessage());
        for (Throwable c = t.getCause(); c != null; c = c.getCause())
            bad("  cause: " + c.getClass().getName() + ": " + c.getMessage());
        StringWriter sw = new StringWriter();
        t.printStackTrace(new PrintWriter(sw));
        for (String line : sw.toString().split("\n")) p("  | " + line);
    }

    static void runOne(String label, String... cmd) {
        p("> " + label + " " + String.join(" ", cmd));
        long t0 = System.currentTimeMillis();
        try {
            ProcessBuilder pb = new ProcessBuilder(cmd);
            pb.redirectErrorStream(true);
            Process pr = pb.start();
            p("< started pid=" + pr.pid() + " alive=" + pr.isAlive());
            Future<String> out = drain(pr.getInputStream(), label);
            boolean done = pr.waitFor(60, TimeUnit.SECONDS);
            p("< waitFor done=" + done + " ms=" + (System.currentTimeMillis() - t0));
            String s = out.get(10, TimeUnit.SECONDS);
            for (String line : s.split("\n")) if (!line.isEmpty()) p("  child| " + line);
            if (done) p("< exitValue=" + pr.exitValue());
            else { p("< still alive, destroying"); pr.destroyForcibly(); }
        } catch (Throwable t) {
            show(label, t);
        }
    }

    public static void main(String[] args) throws Exception {
        watchdog(150_000);
        p("start P1Spawn jvm=" + System.getProperty("java.vm.name")
          + " ver=" + System.getProperty("java.version"));
        p("launchMechanism=" + System.getProperty("jdk.lang.Process.launchMechanism", "(default)")
          + " java.home=" + System.getProperty("java.home")
          + " cwd=" + new File(".").getAbsolutePath());

        /* 0. What the parent can see of the binary it is about to exec.  A
              launch failure that is really "the file is not on the image" must
              not be reported as "exec is broken". */
        for (String f : new String[]{"/LEAF.EXE", "/JDK/bin/java", "/JDK/lib/jspawnhelper"}) {
            Path q = Paths.get(f);
            String seen;
            try {
                seen = Files.exists(q) ? ("exists size=" + Files.size(q)
                        + " exec=" + Files.isExecutable(q)) : "MISSING";
            } catch (Throwable t) { seen = "stat failed " + t; }
            p("path " + f + " -> " + seen);
        }

        p("getpid-equivalent: ProcessHandle.current().pid()=" + ProcessHandle.current().pid());

        runOne("native-leaf-on-argv", "/LEAF.EXE", "--exit=42", "--sig=handle");
        runOne("absolute-java-version", "/JDK/bin/java", "-version");
        runOne("relative-java-version", "JDK/bin/java", "-version");
        runOne("jspawnhelper-direct", "/JDK/lib/jspawnhelper", "0");

        p("SUMMARY P1Spawn reached end of main");
        System.out.flush();
        Runtime.getRuntime().halt(0);
    }
}
