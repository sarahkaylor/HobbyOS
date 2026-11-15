import java.nio.file.*;

/** Second wave line: a separate process reads the file.  Waves run
 *  sequentially ([ARRIVED] A prints before [WAVE-TRY] B), so this is a real
 *  cross-process durability check, not a shared-page-cache illusion. */
public class MMapRead {
    public static void main(String[] a) throws Exception {
        byte[] b = Files.readAllBytes(Paths.get("/JDKTMP/mm.txt"));
        String head = new String(b, 0, 8, "US-ASCII");
        String tail = new String(b, b.length - 8, 8, "US-ASCII");
        System.out.println("MMAPR size=" + b.length + " head=" + head + " tail=" + tail);
        boolean persisted = head.startsWith("ZZZZ") && tail.endsWith("QQQQ");
        System.out.println("MMAPR VERDICT " + (persisted ? "PASS-reached-disk"
            : "FAIL-not-persisted (the page cache does not write through)"));
    }
}
