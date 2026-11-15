import java.lang.reflect.*;
import java.util.*;

/** Runs a subset of OpenJDK's OWN test sources (test/jdk/java/lang/{String,
 *  Integer,Math}) inside HobbyOS.  One JVM, one boot: the wave dispatches
 *  programs concurrently and the console is the only witness, so the suite has
 *  to be a single process.
 *
 *  Honest limits, stated because they change what a PASS means here:
 *   - tests share one VM (jtreg forks each test), so a test that leaks static
 *     state can mask another's failure;
 *   - each test's main() runs on a daemon thread with a timeout, so a hang is
 *     reported instead of wedging the boot;
 *   - tests that call System.exit are excluded by construction (the runner
 *     would die with them). */
public class OpenJdkTests {
    static final String[] TESTS = { "Chars", "ContentEquals", "EqualsIgnoreCase", "HashCode", "Indent", "IsBlank", "Lines", "Split", "Strip", "Transform", "Decode", "ParsingTest", "Unsigned", "ValueOf", "AbsPositiveZero", "Clamp", "MinMax", "PowTests", "RoundTests" };
    static final int TIMEOUT_MS = 15000;

    public static void main(String[] args) {
        int pass = 0, fail = 0;
        for (final String name : TESTS) {
            final Throwable[] err = new Throwable[1];
            long t0 = System.nanoTime();
            Thread th = new Thread(() -> {
                try {
                    Class<?> c = Class.forName(name);
                    Method m = c.getMethod("main", String[].class);
                    m.invoke(null, (Object) new String[0]);
                } catch (InvocationTargetException e) {
                    err[0] = e.getCause() != null ? e.getCause() : e;
                } catch (Throwable e) {
                    err[0] = e;
                }
            }, "jdktest-" + name);
            th.setDaemon(true);
            th.start();
            try { th.join(TIMEOUT_MS); } catch (InterruptedException ie) { /* ignore */ }
            long ms = (System.nanoTime() - t0) / 1000000L;
            if (th.isAlive()) {
                System.out.println("JDKTEST " + name + " HANG after " + TIMEOUT_MS + "ms");
                fail++;
            } else if (err[0] != null) {
                String msg = String.valueOf(err[0]);
                if (err[0].getStackTrace().length > 0)
                    msg += " @" + err[0].getStackTrace()[0];
                System.out.println("JDKTEST " + name + " FAIL " + msg);
                fail++;
            } else {
                System.out.println("JDKTEST " + name + " PASS (" + ms + "ms)");
                pass++;
            }
        }
        System.out.println("JDK-SUITE SUMMARY pass=" + pass + " fail=" + fail + " total=" + TESTS.length);
    }
}
