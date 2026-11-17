import java.io.RandomAccessFile;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.*;

/** Does a write through a MAP_SHARED mapping ever reach the file?  The claim in
 *  docs/java.md is that it does not (file_obj.c has no write path) and that
 *  msync()'s 0 is therefore a lie.  That was a source read, never a boot.
 *  MMapRead (the next wave line, so a DIFFERENT process) reads the bytes back. */
public class MMapWrite {
    public static void main(String[] a) throws Exception {
        Path f = Paths.get("/JDKTMP/mm.txt");
        byte[] init = new byte[4096];
        java.util.Arrays.fill(init, (byte) 'A');
        Files.write(f, init);
        try (RandomAccessFile raf = new RandomAccessFile(f.toFile(), "rw");
             FileChannel ch = raf.getChannel()) {
            MappedByteBuffer b = ch.map(FileChannel.MapMode.READ_WRITE, 0, 4096);
            for (int i = 0; i < 4; i++) b.put(i, (byte) 'Z');
            for (int i = 4092; i < 4096; i++) b.put(i, (byte) 'Q');
            boolean shared = true;
            System.out.println("MMAPW mapped 4096 rw wrote ZZZZ@0 QQQQ@4092");
            b.force();
            System.out.println("MMAPW force() returned");
            /* what the mapping itself sees */
            System.out.println("MMAPW in-mapping b[0]=" + (char) b.get(0)
                + " b[4095]=" + (char) b.get(4095));
        }
        System.out.println("MMAPW DONE");
    }
}
