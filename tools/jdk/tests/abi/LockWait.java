import java.nio.channels.*;
import java.nio.file.*;

/** F_SETLKW must BLOCK.  The holder is a separate process (LOCKHOLD.EXE), which
 *  is a deliberate exception to one-program-per-boot: this kernel models record
 *  locks per PROCESS, so a second thread in the same VM can never conflict and
 *  would prove nothing.  Two concurrent processes is the configuration that can
 *  wedge on proc_lock; if this boot hangs, that is the finding. */
public class LockWait {
    public static void main(String[] a) throws Exception {
        Path f = Paths.get("/JDKTMP/lk.txt");
        Files.writeString(f, "x");
        try (FileChannel ch = FileChannel.open(f, StandardOpenOption.WRITE)) {
            long t0 = System.currentTimeMillis();
            FileLock l = ch.lock();
            long ms = System.currentTimeMillis() - t0;
            System.out.println("LOCKWAIT acquired=true waitedMs=" + ms);
            System.out.println("LOCKWAIT VERDICT "
                + (ms >= 800 ? "PASS-blocked" : "FAIL-returned-immediately"));
            l.release();
        }
    }
}
