import java.nio.file.*;

/** Plain Files.write, no mapping anywhere.  Two later wave lines read it, so the
 *  question is whether a write by one process is even visible to the next one in
 *  the SAME boot -- the mmap case is not worth blaming until this is answered. */
public class PlainWrite {
    public static void main(String[] a) throws Exception {
        Path p = Paths.get("/JDKTMP/x.txt");
        byte[] b = new byte[4096];
        java.util.Arrays.fill(b, (byte) 'A');
        Files.write(p, b);
        System.out.println("PW wrote x.txt size=" + Files.size(p));
    }
}
