import java.nio.file.*;

public class PlainRead {
    public static void main(String[] a) throws Exception {
        Path p = Paths.get("/JDKTMP/x.txt");
        if (!Files.exists(p)) { System.out.println("PR x.txt absent"); return; }
        long n = Files.size(p);
        byte[] b = Files.readAllBytes(p);
        System.out.println("PR size=" + n + " read=" + b.length
            + " first=" + (b.length > 0 ? (char) b[0] : '?')
            + " VERDICT " + (b.length == 4096 ? "visible" : "NOT-VISIBLE"));
    }
}
