import java.nio.file.*;
import java.io.*;

/** Two questions, one boot.
 *
 *  (1) /JDKTMP/mm.txt was written through a MAP_SHARED mapping in the PREVIOUS
 *      boot and force()d; the reader in that boot saw 0 bytes.  The disk still
 *      holds whatever survived, so stat it now: 0 means the mmap path lost it,
 *      4096 with ZZZZ/QQQQ means it landed but the reader raced the flush.
 *  (2) A plain Files.write of 4096 bytes with no mapping at all, then read back
 *      in the same process.  If THAT is 0 too, the mmap story is a red herring
 *      and FAT16 dirent size updates are the bug.
 */
public class FileDurability {
    static void dump(String label, Path p) throws Exception {
        if (!Files.exists(p)) { System.out.println(label + " absent"); return; }
        long n = Files.size(p);
        byte[] b = Files.readAllBytes(p);
        String head = b.length >= 4 ? new String(b, 0, 4, "US-ASCII") : "<short>";
        String tail = b.length >= 4 ? new String(b, b.length - 4, 4, "US-ASCII") : "<short>";
        System.out.println(label + " size=" + n + " read=" + b.length
            + " head=" + head + " tail=" + tail);
    }
    public static void main(String[] a) throws Exception {
        dump("DUR mm.txt (written via mapping last boot)", Paths.get("/JDKTMP/mm.txt"));

        Path plain = Paths.get("/JDKTMP/plain.txt");
        byte[] init = new byte[4096];
        java.util.Arrays.fill(init, (byte) 'P');
        Files.write(plain, init);
        System.out.println("DUR plain.txt wrote 4096, Files.size now=" + Files.size(plain));
        try (InputStream in = new FileInputStream(plain.toFile())) {
            byte[] back = in.readNBytes(4096);
            System.out.println("DUR plain.txt reread=" + back.length
                + " first=" + (back.length > 0 ? (char) back[0] : '?'));
        }
        /* "size > 0" was the wrong test and printed "mmap=landed" for a file whose
           mapped bytes were gone -- the file landed, the writes did not.  The only
           verdict worth printing is whether the MAPPED bytes are on disk. */
        byte[] mm = Files.exists(Paths.get("/JDKTMP/mm.txt"))
            ? Files.readAllBytes(Paths.get("/JDKTMP/mm.txt")) : new byte[0];
        boolean mappedBytesPresent = mm.length >= 4096
            && mm[0] == (byte) 'Z' && mm[4095] == (byte) 'Q';
        System.out.println("DUR VERDICT mapped-bytes="
            + (mappedBytesPresent ? "PRESENT" : "ABSENT (write-through missing)")
            + " plain=" + (Files.size(plain) == 4096 ? "ok" : "BROKEN"));
    }
}
