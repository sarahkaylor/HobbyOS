import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.io.RandomAccessFile;

/** Narrows the 0-byte anomaly.  Plain writes are visible cross-process in one
 *  boot (obj/jdk/plainvis.log), so the trigger is somewhere in mapping a file.
 *  Steps print the size after EACH step so the collapse is attributable:
 *  create -> map -> write -> force -> unmap -> close.  Two PlainRead wave lines
 *  then read it as a different process. */
public class MMapTouch {
    public static void main(String[] a) throws Exception {
        Path p = Paths.get("/JDKTMP/t.txt");
        byte[] b = new byte[4096];
        java.util.Arrays.fill(b, (byte) 'A');
        Files.write(p, b);
        System.out.println("MT after-write size=" + Files.size(p));

        RandomAccessFile raf = new RandomAccessFile(p.toFile(), "rw");
        FileChannel ch = raf.getChannel();
        MappedByteBuffer m = ch.map(FileChannel.MapMode.READ_WRITE, 0, 4096);
        System.out.println("MT after-map size=" + Files.size(p));
        m.put(0, (byte) 'Z');
        m.force();
        System.out.println("MT after-force size=" + Files.size(p)
            + " byte0=" + (char) m.get(0));

        ch.close();
        raf.close();
        System.out.println("MT after-close size=" + Files.size(p));
        try {
            byte[] back = Files.readAllBytes(p);
            System.out.println("MT in-process-reread len=" + back.length
                + " first=" + (back.length > 0 ? (char) back[0] : '?'));
        } catch (Exception e) {
            System.out.println("MT in-process-reread threw " + e);
        }
    }
}
