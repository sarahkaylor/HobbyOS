import java.io.*;
import java.nio.channels.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.net.*;

/**
 * FileProbe -- one step per line, so a failure names the operation instead of
 * arriving as a single exception from inside the nio wrapper.  Every step
 * prints OK/FAIL with the errno-bearing message, and the stack on failure.
 */
public class FileProbe {
    static int fails = 0;

    static void step(String name, Runnable r) {
        try {
            r.run();
            System.out.println("OK   " + name);
        } catch (Throwable t) {
            fails++;
            System.out.println("FAIL " + name + ": " + t);
            StackTraceElement[] st = t.getStackTrace();
            for (int i = 0; i < st.length && i < 6; i++)
                System.out.println("       at " + st[i]);
        }
    }

    public static void main(String[] a) throws Exception {
        String dirName = a.length > 0 ? a[0] : "/JDKTMP";
        Path dir = Paths.get(dirName);
        System.out.println("FileProbe dir=" + dirName + " exists=" + Files.exists(dir)
                + " cwd=" + System.getProperty("user.dir"));

        step("mkdirs", () -> { try { Files.createDirectories(dir); }
                               catch (IOException e) { throw new UncheckedIOException(e); } });
        final Path f = dir.resolve("p.txt");
        step("write", () -> { try { Files.writeString(f, "abc\n", StandardCharsets.UTF_8); }
                              catch (IOException e) { throw new UncheckedIOException(e); } });
        step("read", () -> { try {
                String s = Files.readString(f, StandardCharsets.UTF_8);
                if (!s.equals("abc\n")) throw new IllegalStateException("got " + s);
            } catch (IOException e) { throw new UncheckedIOException(e); } });
        step("size", () -> { try {
                if (Files.size(f) != 4) throw new IllegalStateException("size " + Files.size(f));
            } catch (IOException e) { throw new UncheckedIOException(e); } });
        step("append-channel", () -> { try (FileChannel ch = FileChannel.open(f,
                StandardOpenOption.WRITE, StandardOpenOption.APPEND)) {
                ch.write(java.nio.ByteBuffer.wrap("de".getBytes(StandardCharsets.UTF_8)));
            } catch (IOException e) { throw new UncheckedIOException(e); } });
        step("pread", () -> { try (FileChannel ch = FileChannel.open(f, StandardOpenOption.READ)) {
                java.nio.ByteBuffer bb = java.nio.ByteBuffer.allocate(3);
                int n = ch.read(bb);
                if (n != 3) throw new IllegalStateException("n=" + n);
            } catch (IOException e) { throw new UncheckedIOException(e); } });
        step("list", () -> { try (var s = Files.list(dir)) {
                long c = s.count();
                System.out.println("       entries=" + c);
            } catch (IOException e) { throw new UncheckedIOException(e); } });
        final Path g = dir.resolve("p2.txt");
        step("move", () -> { try { Files.move(f, g); }
                             catch (IOException e) { throw new UncheckedIOException(e); } });
        step("exists-after-move", () -> {
                if (!Files.exists(g)) throw new IllegalStateException("missing " + g); });
        step("delete", () -> { try { Files.delete(g); }
                               catch (IOException e) { throw new UncheckedIOException(e); } });
        step("deleted", () -> {
                if (Files.exists(g)) throw new IllegalStateException("still there"); });
        step("tempfile", () -> { try { Path t = Files.createTempFile(dir, "tp", ".tmp");
                                       Files.writeString(t, "x");
                                       long n = Files.size(t);
                                       Files.delete(t);
                                       if (n != 1) throw new IllegalStateException("n=" + n); }
                                catch (IOException e) { throw new UncheckedIOException(e); } });
        // Does ANY directory enumerate?  A listing failure that is specific to
        // one directory says something about how that directory was created; a
        // failure for every directory says the O_DIRECTORY/open+getdents path
        // itself is wrong.
        for (String d : new String[] { "/", "/JDK", "/JDK/lib", "/C", "/JDKTMP" }) {
            final String dn = d;
            step("list " + dn, () -> {
                try (var s = Files.list(java.nio.file.Paths.get(dn))) {
                    long c = s.count();
                    if (c < 0) throw new IllegalStateException("neg");
                    System.out.println("       " + dn + " entries=" + c);
                } catch (IOException e) { throw new UncheckedIOException(e); }
            });
        }
        System.out.println("FILEPROBE fails=" + fails);
    }
}
