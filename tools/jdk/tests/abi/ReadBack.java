import java.nio.file.*;

/** Cross-boot durability: t.txt was written through a MAP_SHARED mapping and
 *  force()d in the PREVIOUS boot.  Same-boot reads only prove coherence (the
 *  cache was invalidated so the read path re-read from the disk it was just
 *  given); only a fresh boot proves the bytes are in the file system. */
public class ReadBack {
    public static void main(String[] a) throws Exception {
        for (String name : new String[] {"/JDKTMP/t.txt", "/JDKTMP/x.txt"}) {
            Path p = Paths.get(name);
            if (!Files.exists(p)) { System.out.println("RB " + name + " absent"); continue; }
            byte[] b = Files.readAllBytes(p);
            System.out.println("RB " + name + " size=" + b.length
                + " first=" + (b.length > 0 ? (char) b[0] : '?')
                + " last=" + (b.length > 0 ? (char) b[b.length - 1] : '?'));
        }
        byte[] t = Files.exists(Paths.get("/JDKTMP/t.txt"))
            ? Files.readAllBytes(Paths.get("/JDKTMP/t.txt")) : new byte[0];
        System.out.println("RB VERDICT mapped-byte="
            + (t.length >= 4096 && t[0] == (byte) 'Z' ? "DURABLE" : "LOST"));
    }
}
