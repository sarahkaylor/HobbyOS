import java.nio.file.*;

/** FAT16 has no execute bit; this layer reported every file as 0644, so
 *  Files.isExecutable() was false for every binary.  Linux's vfat mount
 *  reports files as executable (mode=0777 & ~umask), and this now matches. */
public class ExecBit {
    public static void main(String[] a) throws Exception {
        Path j = Paths.get("/JDK/bin/java");
        boolean x = Files.isExecutable(j);
        System.out.println("EXECBIT isExecutable=" + x
            + " canExecute=" + j.toFile().canExecute()
            + " size=" + Files.size(j));
        System.out.println("EXECBIT VERDICT " + (x ? "PASS" : "FAIL"));
    }
}
