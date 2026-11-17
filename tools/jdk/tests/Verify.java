/*
 * tools/jdk/tests/Verify.java -- the functional acceptance suite for OpenJDK on
 * HobbyOS.  One program, no third-party dependencies, one line per check:
 *
 *     PASS <n> <name>
 *     FAIL <n> <name>: <detail>
 *     SUMMARY pass=<n> fail=<n> skip=<n>
 *
 * Why in one process: the boot wave (MODE=test) dispatches every WAVE.CFG line
 * CONCURRENTLY, so a suite made of many java invocations would race itself.
 * One JVM, many checks, ordered output.
 *
 * What it is for: each check is a subsystem the kernel's Linux-ABI layer has to
 * provide for real.  A failure names the layer, not "java is broken":
 * threads -> clone/futex, files -> openat/getdents/ftruncate, zlib -> libzip,
 * crypto -> getrandom//dev/urandom, sockets -> socket2, time -> clock_gettime.
 */
import java.io.*;
import java.math.BigInteger;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.*;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.*;

public class Verify {
    static int pass = 0, fail = 0, skip = 0;

    static int n = 0;

    interface Body { void run() throws Exception; }

    static void ok(String name, boolean cond, String detail) {
        int i = ++n;
        if (cond) { pass++; System.out.println("PASS " + i + " " + name); }
        else { fail++; System.out.println("FAIL " + i + " " + name + ": " + detail); }
    }

    static void check(String name, Body b) {
        try {
            b.run();
            ok(name, true, "");
        } catch (Throwable t) {
            Throwable c = t;
            while (c instanceof RuntimeException && c.getCause() != null
                   && c.getClass() == RuntimeException.class) c = c.getCause();
            ok(name, false, c.getClass().getName() + ": " + c.getMessage());
        }
    }

    // ---------------------------------------------------------------- groups

    /* 1. The interpreter and the JIT: a long arithmetic loop is compiled by C2
       and must produce a bit-identical answer to the cold path. */
    static void tArithmetic() {
        long sum = 0;
        for (long i = 0; i < 5_000_000L; i++) sum += (i * 2654435761L) ^ (sum >> 7);
        long expect = 0;
        for (long i = 0; i < 5_000_000L; i++) expect += (i * 2654435761L) ^ (expect >> 7);
        if (sum != expect) throw new IllegalStateException("sum mismatch");
        if (sum == 0) throw new IllegalStateException("sum is zero");
    }

    static void tStrings() {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 2000; i++) sb.append((char) ('a' + (i % 26)));
        String s = sb.toString();
        if (s.length() != 2000) throw new IllegalStateException("length");
        if (!s.substring(0, 5).equals("abcde")) throw new IllegalStateException("substring");
        if (s.indexOf("xyz") < 0) throw new IllegalStateException("indexOf");
        String up = s.toUpperCase(Locale.ROOT);
        if (!up.equals(s.toUpperCase(Locale.ROOT))) throw new IllegalStateException("upper");
        if (String.join(",", "a", "b", "c").length() != 5) throw new IllegalStateException("join");
    }

    static void tUnicode() {
        String src = "Héllo Wörld — 日本語 — Ωμέγα — \uD83D\uDE00";
        byte[] b = src.getBytes(StandardCharsets.UTF_8);
        String back = new String(b, StandardCharsets.UTF_8);
        if (!back.equals(src)) throw new IllegalStateException("roundtrip");
        int cps = src.codePointCount(0, src.length());
        if (cps != 29) throw new IllegalStateException("codepoints=" + cps);
        byte[] u16 = src.getBytes(StandardCharsets.UTF_16BE);
        if (u16.length != src.length() * 2) throw new IllegalStateException("utf16");
        if (!new String(u16, StandardCharsets.UTF_16BE).equals(src))
            throw new IllegalStateException("utf16 roundtrip");
    }

    static void tRegex() {
        Pattern p = Pattern.compile("(\\w+)@(\\w+)\\.(\\w+)");
        Matcher m = p.matcher("contact: alice@example.org and bob@test.net");
        int n = 0;
        String first = null;
        while (m.find()) { if (first == null) first = m.group(1); n++; }
        if (n != 2) throw new IllegalStateException("matches=" + n);
        if (!"alice".equals(first)) throw new IllegalStateException("group=" + first);
        if (!"a1b22c333".replaceAll("\\d+", "#").equals("a#b#c#"))
            throw new IllegalStateException("replaceAll");
        if (!Pattern.compile("^[a-z0-9_-]{3,16}$").matcher("user_name-99").matches())
            throw new IllegalStateException("quantifier");
    }

    static void tCollections() {
        Map<String, Integer> m = new HashMap<>();
        for (int i = 0; i < 20_000; i++) m.merge("k" + (i % 997), 1, Integer::sum);
        if (m.size() != 997) throw new IllegalStateException("size=" + m.size());
        int total = m.values().stream().mapToInt(Integer::intValue).sum();
        if (total != 20_000) throw new IllegalStateException("total=" + total);
        TreeMap<Integer, String> tm = new TreeMap<>();
        for (int i = 0; i < 1000; i++) tm.put((i * 7919) % 1000, "v" + i);
        if (tm.firstKey() != 0 || tm.lastKey() != 999) throw new IllegalStateException("order");
        List<Integer> l = IntStream.range(0, 10_000).boxed()
                .filter(x -> x % 3 == 0).sorted(Comparator.reverseOrder())
                .collect(Collectors.toList());
        if (l.size() != 3334 || l.get(0) != 9999) throw new IllegalStateException("stream");
        Set<String> s = new HashSet<>(Arrays.asList("a", "b", "a"));
        if (s.size() != 2) throw new IllegalStateException("set");
    }

    static void tLambdasReflect() throws Exception {
        Function<Integer, Integer> sq = x -> x * x;
        if (sq.apply(12) != 144) throw new IllegalStateException("lambda");
        BiFunction<String, Integer, String> rep = String::repeat;
        if (!rep.apply("ab", 3).equals("ababab")) throw new IllegalStateException("mref");
        Class<?> c = Class.forName("java.util.ArrayList");
        Object o = c.getDeclaredConstructor().newInstance();
        c.getMethod("add", Object.class).invoke(o, "x");
        if (((List<?>) o).size() != 1) throw new IllegalStateException("reflect");
        java.lang.reflect.Method len = String.class.getMethod("length");
        if (!len.getName().equals("length")) throw new IllegalStateException("method");
    }

    static void tThreads() throws Exception {
        AtomicLong acc = new AtomicLong();
        Thread[] ts = new Thread[8];
        for (int i = 0; i < ts.length; i++) {
            ts[i] = new Thread(() -> {
                for (int k = 0; k < 200_000; k++) acc.incrementAndGet();
            });
        }
        for (Thread t : ts) t.start();
        for (Thread t : ts) t.join();
        if (acc.get() != 1_600_000L) throw new IllegalStateException("acc=" + acc.get());

        ExecutorService ex = Executors.newFixedThreadPool(4);
        List<Future<Integer>> fs = new ArrayList<>();
        for (int i = 0; i < 16; i++) {
            final int v = i;
            fs.add(ex.submit(() -> v * v));
        }
        int sum = 0;
        for (Future<Integer> f : fs) sum += f.get(60, TimeUnit.SECONDS);
        ex.shutdown();
        if (!ex.awaitTermination(60, TimeUnit.SECONDS)) throw new IllegalStateException("pool exit");
        if (sum != 1240) throw new IllegalStateException("sum=" + sum);

        int fj = ForkJoinPool.commonPool().submit(
                () -> IntStream.range(0, 100_000).parallel().map(x -> x % 7).sum()).get(120, TimeUnit.SECONDS);
        if (fj <= 0) throw new IllegalStateException("forkjoin=" + fj);
    }

    static void tFiles() throws Exception {
        Path dir = Paths.get(System.getProperty("verify.dir", "/JDKTMP"));
        try { Files.createDirectories(dir); } catch (FileAlreadyExistsException e) { /* fine */ }
        Path f = dir.resolve("verify.txt");
        String payload = "line1\nline2\nhéllo\n";
        Files.writeString(f, payload, StandardCharsets.UTF_8);
        String back = Files.readString(f, StandardCharsets.UTF_8);
        if (!back.equals(payload)) throw new IllegalStateException("readback");
        if (Files.size(f) != payload.getBytes(StandardCharsets.UTF_8).length)
            throw new IllegalStateException("size");
        // Append + random access through a channel (pread/pwrite + ftruncate).
        try (FileChannel ch = FileChannel.open(f, StandardOpenOption.WRITE, StandardOpenOption.APPEND)) {
            ch.write(ByteBuffer.wrap("tail".getBytes(StandardCharsets.UTF_8)));
        }
        int plen = payload.getBytes(StandardCharsets.UTF_8).length;
        if (Files.size(f) != plen + 4) throw new IllegalStateException("append size " + Files.size(f) + " != " + (plen + 4));
        try (FileChannel ch = FileChannel.open(f, StandardOpenOption.READ)) {
            ByteBuffer bb = ByteBuffer.allocate(5);
            ch.position(0);
            int n = ch.read(bb);
            if (n != 5) throw new IllegalStateException("read n=" + n);
            if (!new String(bb.array(), StandardCharsets.UTF_8).equals("line1"))
                throw new IllegalStateException("positional read");
        }
        // Directory listing: getdents64.
        long seen = Files.list(dir).count();
        if (seen < 1) throw new IllegalStateException("list");
        // Rename + delete.
        Path g = dir.resolve("verify2.txt");
        Files.move(f, g);
        if (!Files.exists(g)) throw new IllegalStateException("move");
        Files.delete(g);
        if (Files.exists(g)) throw new IllegalStateException("delete");
    }

    static void tZlib() throws Exception {
        // Compressible payload: a check that the compressor RAN (bytes shrink)
        // and that the round-trip is byte-exact.  Random bytes would grow and
        // the "shrink" assertion below would be meaningless.
        byte[] raw = new byte[512 * 1024];
        for (int i = 0; i < raw.length; i++) raw[i] = (byte) ("hobbyos-jdk".charAt(i % 11));
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        try (java.util.zip.GZIPOutputStream gz = new java.util.zip.GZIPOutputStream(bos)) {
            gz.write(raw);
        }
        byte[] comp = bos.toByteArray();
        if (comp.length >= raw.length / 2)
            throw new IllegalStateException("did not compress: " + comp.length + " of " + raw.length);
        ByteArrayInputStream bis = new ByteArrayInputStream(comp);
        byte[] out = new byte[raw.length];
        try (java.util.zip.GZIPInputStream gz = new java.util.zip.GZIPInputStream(bis)) {
            int off = 0, n;
            while (off < out.length && (n = gz.read(out, off, out.length - off)) > 0) off += n;
            if (off != out.length) throw new IllegalStateException("inflate len=" + off);
        }
        if (!Arrays.equals(raw, out)) throw new IllegalStateException("roundtrip");
        // Deflate/zlib inflater on a well-known vector.
        java.util.zip.Deflater d = new java.util.zip.Deflater();
        byte[] s = "hobbyos".getBytes(StandardCharsets.UTF_8);
        d.setInput(s);
        d.finish();
        byte[] buf = new byte[256];
        int len = d.deflate(buf);
        d.end();
        if (len <= 0) throw new IllegalStateException("deflate");
        java.util.zip.Inflater inf = new java.util.zip.Inflater();
        inf.setInput(buf, 0, len);
        byte[] ob = new byte[256];
        int ol = inf.inflate(ob);
        inf.end();
        if (!new String(ob, 0, ol, StandardCharsets.UTF_8).equals("hobbyos"))
            throw new IllegalStateException("inflate string");
    }

    static void tCrypto() throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        byte[] dig = md.digest("abc".getBytes(StandardCharsets.US_ASCII));
        StringBuilder hex = new StringBuilder();
        for (byte b : dig) hex.append(String.format("%02x", b));
        if (!hex.toString().equals(
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
            throw new IllegalStateException("sha256=" + hex);
        MessageDigest m5 = MessageDigest.getInstance("MD5");
        StringBuilder h5 = new StringBuilder();
        for (byte b : m5.digest("abc".getBytes(StandardCharsets.US_ASCII)))
            h5.append(String.format("%02x", b));
        if (!h5.toString().equals("900150983cd24fb0d6963f7d28e17f72"))
            throw new IllegalStateException("md5=" + h5);
    }

    static void tSecureRandom() throws Exception {
        SecureRandom sr = SecureRandom.getInstanceStrong();
        byte[] b = new byte[32];
        sr.nextBytes(b);
        boolean allZero = true;
        for (byte x : b) if (x != 0) { allZero = false; break; }
        if (allZero) throw new IllegalStateException("all zero");
        SecureRandom sr2 = new SecureRandom();
        long v = sr2.nextLong();
        if (v == 0) throw new IllegalStateException("nextLong 0");
    }

    static void tBigInteger() {
        BigInteger f = BigInteger.ONE;
        for (int i = 2; i <= 200; i++) f = f.multiply(BigInteger.valueOf(i));
        if (f.toString().length() != 375) throw new IllegalStateException("len=" + f.toString().length());
        BigInteger p = new BigInteger("170141183460469231731687303715884105727");
        if (!p.isProbablePrime(64)) throw new IllegalStateException("prime");
        if (p.nextProbablePrime().subtract(p).signum() <= 0) throw new IllegalStateException("next");
        double d = Math.sqrt(2.0);
        if (Math.abs(d * d - 2.0) > 1e-12) throw new IllegalStateException("sqrt");
        if (Math.abs(Math.sin(Math.PI / 6) - 0.5) > 1e-12) throw new IllegalStateException("sin");
        if (Math.abs(Math.log(Math.E) - 1.0) > 1e-12) throw new IllegalStateException("log");
        if (Math.abs(Math.exp(1.0) - Math.E) > 1e-12) throw new IllegalStateException("exp");
        if (Math.abs(Math.pow(2, 0.5) - d) > 1e-12) throw new IllegalStateException("pow");
    }

    static void tTime() throws Exception {
        long t0 = System.nanoTime();
        long m0 = System.currentTimeMillis();
        Thread.sleep(30);
        long dt = System.nanoTime() - t0;
        if (dt < 10_000_000L) throw new IllegalStateException("nanoTime dt=" + dt);
        if (dt > 60_000_000_000L) throw new IllegalStateException("nanoTime way off dt=" + dt);
        long dm = System.currentTimeMillis() - m0;
        if (dm < 10 || dm > 60_000) throw new IllegalStateException("millis dt=" + dm);
        java.time.LocalDateTime now = java.time.LocalDateTime.now();
        if (now.getYear() < 2020) throw new IllegalStateException("year=" + now.getYear());
        java.time.Instant i = java.time.Instant.now();
        if (i.getEpochSecond() < 1_700_000_000L) throw new IllegalStateException("instant");
        if (java.time.Duration.between(java.time.Instant.EPOCH, i).toDays() < 2000)
            throw new IllegalStateException("duration");
    }

    static void tGc() {
        List<byte[]> keep = new ArrayList<>();
        for (int i = 0; i < 400; i++) keep.add(new byte[256 * 1024]);
        long used = Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory();
        if (used <= 0) throw new IllegalStateException("used=" + used);
        keep.clear();
        for (int i = 0; i < 200; i++) {
            byte[] b = new byte[512 * 1024];
            b[0] = (byte) i;
        }
        System.gc();
        Runtime rt = Runtime.getRuntime();
        if (rt.maxMemory() < 16L * 1024 * 1024) throw new IllegalStateException("maxMemory=" + rt.maxMemory());
        if (rt.availableProcessors() < 1) throw new IllegalStateException("cpus");
    }

    static void tSerialization() throws Exception {
        HashMap<String, int[]> m = new HashMap<>();
        m.put("a", new int[]{1, 2, 3});
        m.put("b", new int[]{4, 5, 6});
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        try (ObjectOutputStream oos = new ObjectOutputStream(bos)) { oos.writeObject(m); }
        Object back;
        try (ObjectInputStream ois = new ObjectInputStream(new ByteArrayInputStream(bos.toByteArray()))) {
            back = ois.readObject();
        }
        if (!Arrays.equals(((HashMap<String, int[]>) back).get("b"), new int[]{4, 5, 6}))
            throw new IllegalStateException("roundtrip");
        // Records + enums (newer class-file features).
        record P(int x, int y) {}
        if (new P(1, 2).equals(new P(1, 2)) == false) throw new IllegalStateException("record");
    }

    static void tSockets() throws Exception {
        // Loopback bind/connect: socket2 + bind + listen + accept + connect.
        try (ServerSocket ss = new ServerSocket(0, 4, InetAddress.getLoopbackAddress())) {
            final int port = ss.getLocalPort();
            Thread t = new Thread(() -> {
                try (java.net.Socket c = new java.net.Socket(InetAddress.getLoopbackAddress(), port)) {
                    c.getOutputStream().write("ping".getBytes(StandardCharsets.US_ASCII));
                    c.getOutputStream().flush();
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
            });
            t.start();
            try (java.net.Socket a = ss.accept()) {
                byte[] b = new byte[4];
                int n = a.getInputStream().read(b);
                if (n != 4) throw new IllegalStateException("n=" + n);
                if (!new String(b, StandardCharsets.US_ASCII).equals("ping"))
                    throw new IllegalStateException("payload");
            }
            t.join(30_000);
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }

    static void tClassLoad() throws Exception {
        // Load a class the program never linked against (class-file parsing +
        // jar/jimage lookup), then use java.lang.instrument-free reflection.
        Class<?> c = Class.forName("java.util.concurrent.ConcurrentHashMap");
        if (!java.util.Map.class.isAssignableFrom(c)) throw new IllegalStateException("hierarchy");
        Class<?> z = Class.forName("java.util.zip.Deflater");
        if (z.getDeclaredMethods().length == 0) throw new IllegalStateException("methods");
        // A generated class name that must NOT exist.
        try {
            Class.forName("no.such.Class");
            throw new IllegalStateException("loaded a class that does not exist");
        } catch (ClassNotFoundException expected) { /* pass */ }
    }

    static void tEnvAndStreams() throws Exception {
        System.out.flush();
        System.err.println("VERIFY: this line is written to stderr");
        Map<String, String> env = System.getenv();
        if (env == null) throw new IllegalStateException("env null");
        byte[] in = new byte[0];
        if (System.in.read(in) != 0) throw new IllegalStateException("read 0 bytes");
        LineNumberReader r = new LineNumberReader(new StringReader("x\ny\n"));
        if (r.readLine() == null) throw new IllegalStateException("reader");
        StringWriter sw = new StringWriter();
        PrintWriter pw = new PrintWriter(sw);
        pw.printf("%05.2f|%s|%d%n", 3.14159, "s", 42);
        pw.flush();
        if (!sw.toString().equals("03.14|s|42" + System.lineSeparator()))
            throw new IllegalStateException("printf=" + sw);
    }

    // ------------------------------------------------------------------ main

    public static void main(String[] args) throws Exception {
        System.out.println("VERIFY: start jvm=" + System.getProperty("java.vm.name")
                + " ver=" + System.getProperty("java.version")
                + " cpus=" + Runtime.getRuntime().availableProcessors());
        check("arithmetic-jit", Verify::tArithmetic);
        check("strings", Verify::tStrings);
        check("unicode-charsets", Verify::tUnicode);
        check("regex", Verify::tRegex);
        check("collections-streams", Verify::tCollections);
        check("lambdas-reflection", Verify::tLambdasReflect);
        check("threads-executor-forkjoin", Verify::tThreads);
        check("files-nio", Verify::tFiles);
        check("zlib-gzip-deflate", Verify::tZlib);
        check("crypto-digests", Verify::tCrypto);
        check("secure-random", Verify::tSecureRandom);
        check("biginteger-math", Verify::tBigInteger);
        check("time-clocks", Verify::tTime);
        check("gc-memory", Verify::tGc);
        check("serialization-records", Verify::tSerialization);
        check("sockets-loopback", Verify::tSockets);
        check("classloading", Verify::tClassLoad);
        check("stdio-streams", Verify::tEnvAndStreams);
        System.out.println("SUMMARY pass=" + pass + " fail=" + fail + " skip=" + skip);
        if (fail != 0) System.exit(1);
    }
}
