/*
 * tools/jdk/tests/Base64Probe.java -- CVE-2026-114938 ("Java4Shell") regression
 * probe, run inside the guest.
 *
 * What the CVE is: a heap out-of-bounds write in HotSpot's x86-64 AVX-512 VBMI
 * Base64 decode stub (StubGenerator::generate_base64_decodeBlock).  When the
 * tail of the input is shorter than one complete 4-character group and ends in
 * '=', the stub computes output_size = (len/4)*3 = 0, the padding branch
 * decrements it to -1, BZHI leaves the store mask at all ones, and the final
 * evmovdqub writes 64 bytes from a destination that has room for 2.  Java then
 * throws IllegalArgumentException -- after the write.  Upstream fix (JDK-8393212,
 * commit 5177a70b385c) sends such a tail to the scalar loop.
 *
 * What this probe can and cannot prove, stated up front so a green run is not
 * overread:
 *
 *   CAN: that the pinned runtime still rejects the exact trigger shapes with
 *   IllegalArgumentException, still decodes well-formed padded groups, and that
 *   the heap it hands back afterwards is intact (no crash, no silent corruption
 *   of objects allocated before the burst).
 *
 *   CANNOT: reproduce the 62-byte overflow here.  The stub is only generated on
 *   a CPU reporting avx512f+avx512bw+avx512vbmi+bmi2, and the guest runs on
 *   QEMU TCG, which does not expose AVX-512 VBMI.  Record that with
 *   `java -XX:+PrintFlagsFinal -version | grep UseAVX`: a green run under
 *   UseAVX=2 measures the Java-visible contract, not the VBMI code path.  The
 *   evidence for the patch itself is the binary diff in docs/java.md (the fixed
 *   libjvm.so's stub generator gains exactly the two assembler emissions the
 *   upstream patch adds).
 *
 * Shape: one program, one JVM, one line per check (PASS/FAIL), a SUMMARY at the
 * end.  No argv, no files: everything it needs is in the heap it asserts on.
 */
import java.util.Arrays;
import java.util.Base64;
import java.util.Random;

public class Base64Probe {
    static int pass = 0, fail = 0;

    static void ok(boolean cond, String name, String detail) {
        if (cond) {
            pass++;
            System.out.println("B64 PASS " + name + (detail.isEmpty() ? "" : "  " + detail));
        } else {
            fail++;
            System.out.println("B64 FAIL " + name + ": " + detail);
        }
    }

    // Inputs that must throw IllegalArgumentException.  The 65-byte shapes are
    // the published CVE triggers; the short ones are the same defect in
    // miniature (tail < one 4-character group, trailing '=').
    static final String[] MUST_REJECT = {
        "=",                  // 1 byte, pure padding
        "A=",                 // 2 bytes
        "AA=",                // 3 bytes
        "AAAAA",              // 5 bytes, no padding, bad length
        "A".repeat(64) + "=", // 65 bytes -- CVE trigger on 21/25/27
        "A".repeat(64) + "==",
        "A".repeat(128) + "=",// CVE trigger shape on 17
        "A".repeat(64) + "A", // 65 bytes, no padding, bad length
    };

    // Inputs that are well-formed and must decode, padding included -- the
    // shapes a fix that rejects too much would break.
    static final String[] MUST_DECODE = { "AAA=", "AA==", "AAAA", "AQID" };

    static String rejected(String in, Base64.Decoder d) {
        try {
            d.decode(in);
            return null;
        } catch (IllegalArgumentException e) {
            return e.getClass().getSimpleName();
        }
    }

    static byte[] pattern(int n, long seed) {
        byte[] b = new byte[n];
        new Random(seed).nextBytes(b);
        return b;
    }

    public static void main(String[] argv) {
        System.out.println("B64 java.version=" + System.getProperty("java.version")
                + " vm=" + System.getProperty("java.vm.version")
                + " arch=" + System.getProperty("os.arch")
                + " cpus=" + Runtime.getRuntime().availableProcessors());

        Base64.Decoder basic = Base64.getDecoder();
        Base64.Decoder mime = Base64.getMimeDecoder();
        Base64.Decoder url = Base64.getUrlDecoder();
        Base64.Encoder enc = Base64.getEncoder();

        // Allocated BEFORE the reject burst, read back after it: an overwrite
        // past a decode buffer has to land somewhere, and a wrong field here is
        // how it would show up without a crash.
        long[] sentinel = { 0x5ee1L, 42L, -7L, 0xdeadbeefL };

        // --- 1. valid round trips, small and large -------------------------
        byte[] p1 = pattern(48, 1L);
        ok(Arrays.equals(p1, basic.decode(enc.encodeToString(p1))), "roundtrip-basic-48", "");
        byte[] p2 = pattern(8192, 2L);
        ok(Arrays.equals(p2, basic.decode(enc.encodeToString(p2))), "roundtrip-basic-8192",
           "in=" + p2.length);
        String mimeText = enc.encodeToString(p2).replaceAll("(.{76})", "$1\r\n");
        ok(Arrays.equals(p2, mime.decode(mimeText)), "roundtrip-mime-wrapped",
           "chars=" + mimeText.length());
        byte[] p3 = pattern(300, 3L);
        ok(Arrays.equals(p3, url.decode(Base64.getUrlEncoder().encodeToString(p3))),
           "roundtrip-url-300", "");

        // --- 2. every CVE trigger shape throws, none decodes ---------------
        boolean allRejected = true;
        StringBuilder detail = new StringBuilder();
        for (String t : MUST_REJECT) {
            String r = rejected(t, basic);
            allRejected &= (r != null);
            detail.append(t.length()).append(':').append(r).append(' ');
        }
        ok(allRejected, "reject-malformed-tail", detail.toString().trim());

        // --- 3. ... and each rejection leaves the decoder usable ----------
        boolean usable = true;
        for (String t : MUST_REJECT) {
            rejected(t, basic);
            usable &= Arrays.equals(p1, basic.decode(enc.encodeToString(p1)));
        }
        ok(usable, "decoder-usable-after-rejects", "rejects=" + MUST_REJECT.length);

        // --- 4. well-formed padded groups still decode ---------------------
        // A fix that simply refused every tail would pass check 2 and break
        // every real Base64 payload; these are the inputs that catch it.
        boolean decodes = true;
        StringBuilder dd = new StringBuilder();
        for (String t : MUST_DECODE) {
            try {
                int len = basic.decode(t).length;
                dd.append(t).append("->").append(len).append(' ');
                decodes &= (len == expectedLen(t));
            } catch (IllegalArgumentException e) {
                decodes = false;
                dd.append(t).append("->REJECTED ");
            }
        }
        ok(decodes, "decode-wellformed-padded", dd.toString().trim());

        // --- 5. decode into a CALLER-SUPPLIED array ------------------------
        // The shape the stub writes into: Java sizes dst itself, so an overflow
        // lands past it.  Assert the in-bounds prefix is exactly right.
        byte[] dst = new byte[48];
        int n = basic.decode(enc.encode(p1), dst);
        ok(n == 48 && Arrays.equals(p1, dst), "decode-into-supplied-array",
           "n=" + n + " dst=" + dst.length);

        // --- 6. warm the decoder, then hammer it with triggers -------------
        // The stub is selected by C2 once decodeBlock is hot (Calif's PoC warms
        // with 200k valid decodes first).  The point here: the reject path stays
        // correct after the intrinsic is in play, whatever this CPU selected.
        byte[] warm = pattern(64, 9L);
        String warmEnc = enc.encodeToString(warm);
        long sum = 0;
        for (int i = 0; i < 20000; i++)
            sum += basic.decode(warmEnc).length;
        ok(sum == 20000L * warm.length, "warm-20000-valid", "sum=" + sum);

        int rejects = 0;
        for (int i = 0; i < 200; i++)
            for (String t : MUST_REJECT)
                if (rejected(t, basic) != null) rejects++;
        ok(rejects == 200 * MUST_REJECT.length, "burst-rejects-no-crash",
           "rejects=" + rejects);

        // --- 7. heap sanity after the burst --------------------------------
        // Objects allocated BEFORE the burst must still read back exactly, and
        // a fresh allocation must be fully zero.  A 62-byte overwrite past a
        // decode buffer would surface here as a wrong field, not an exception.
        byte[] after = basic.decode(warmEnc);
        ok(Arrays.equals(warm, after), "valid-decode-after-burst", "len=" + after.length);
        byte[] fresh = new byte[96];
        boolean zero = true;
        for (byte b : fresh) zero &= (b == 0);
        ok(zero, "fresh-allocation-zeroed", "len=" + fresh.length);
        ok(Arrays.equals(sentinel, new long[] { 0x5ee1L, 42L, -7L, 0xdeadbeefL }),
           "sentinel-intact", Arrays.toString(sentinel));

        System.out.println("B64 SUMMARY pass=" + pass + " fail=" + fail);
        System.out.println("SUMMARY pass=" + pass + " fail=" + fail + " skip=0");
        if (fail != 0) System.exit(1);
    }

    static int expectedLen(String b64) {
        int pad = 0;
        for (int i = b64.length() - 1; i >= 0 && b64.charAt(i) == '='; i--) pad++;
        return (b64.length() / 4) * 3 - pad;
    }
}
