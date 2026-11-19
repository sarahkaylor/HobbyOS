/*
 * tools/jdk/tests/TcpProbe.java -- does java.net TCP work over the real NIC?
 *
 * The suite's sockets-loopback check (Verify) only proves the in-kernel
 * 127.0.0.1 pair.  This probe goes out the virtio-net device, through QEMU's
 * slirp user-mode network, to an echo server the harness runs on the HOST at
 * 10.0.2.2 (the slirp alias for the host's loopback, the same address
 * tools/run_browser_accept.sh and tools/rw/nfs-export.sh use).  Nothing here
 * touches DNS or the internet: the address is a literal, the server is local,
 * so a green run says "TCP works" and not "the network happened to be up".
 *
 * What each check buys:
 *   resolve-literal     InetAddress parses 10.0.2.2 without a resolver
 *   local-address       the DHCP lease is real: the socket is bound to a
 *                       routable guest address, not 0.0.0.0 and not loopback
 *   connect+echo        SYN/SYN-ACK/ACK completes and a byte-identical
 *                       round trip happens (data both directions)
 *   bulk-8192           segmentation works: 8192 B is 6 x TCP_MAX_PAYLOAD
 *                       (1446) segments, so this is the multi-segment path
 *   reconnect           a second connection to the same server succeeds
 *                       (PCB teardown/port reuse, not a one-shot fluke)
 *   refused-closed-port the stack surfaces the host's RST as ConnectException
 *                       instead of hanging (the negative case: a connect that
 *                       succeeds here would mean connects are not real)
 *
 * Usage: java -cp /C TcpProbe [host] [port]        (default 10.0.2.2 18089)
 * Prints "TCP PASS <name>" / "TCP FAIL <name>: detail" and a TCP SUMMARY.
 */
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ConnectException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.util.Random;

public class TcpProbe {
    static int pass = 0, fail = 0;
    static final int CONNECT_MS = 15000, READ_MS = 30000;

    static void ok(boolean cond, String name, String detail) {
        if (cond) { pass++; System.out.println("TCP PASS " + name + (detail.isEmpty() ? "" : "  " + detail)); }
        else { fail++; System.out.println("TCP FAIL " + name + ": " + detail); }
    }

    static byte[] pattern(int n, long seed) {
        byte[] b = new byte[n];
        new Random(seed).nextBytes(b);
        return b;
    }

    /** Open, write exactly, read exactly, close.  Returns what came back. */
    static byte[] roundTrip(InetAddress dst, int port, byte[] payload) throws Exception {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress(dst, port), CONNECT_MS);
            s.setSoTimeout(READ_MS);
            s.setTcpNoDelay(true);
            OutputStream out = s.getOutputStream();
            out.write(payload);
            out.flush();
            InputStream in = s.getInputStream();
            byte[] got = new byte[payload.length];
            int off = 0;
            while (off < got.length) {
                int n = in.read(got, off, got.length - off);
                if (n < 0) throw new java.io.EOFException("EOF at " + off + "/" + got.length);
                off += n;
            }
            return got;
        }
    }

    public static void main(String[] argv) throws Exception {
        String host = argv.length > 0 ? argv[0] : "10.0.2.2";
        int port = argv.length > 1 ? Integer.parseInt(argv[1]) : 18089;
        System.out.println("TCP java.version=" + System.getProperty("java.version")
                + " peer=" + host + ":" + port);

        // --- 1. literal address, no resolver involved ----------------------
        InetAddress dst = null;
        try {
            dst = InetAddress.getByName(host);
        } catch (Throwable t) {
            ok(false, "resolve-literal", t.toString());
        }
        if (dst == null) { summary(); return; }
        ok(dst.getHostAddress().equals(host) || !dst.isAnyLocalAddress(),
           "resolve-literal", dst.getHostAddress());

        // --- 2. connect + a byte-identical line round trip -----------------
        String nonce = Long.toHexString(new Random().nextLong());
        byte[] line = ("PING " + nonce + "\n").getBytes("UTF-8");
        byte[] echo = null;
        try {
            echo = roundTrip(dst, port, line);
        } catch (Throwable t) {
            ok(false, "connect-echo", t.getClass().getSimpleName() + ": " + t.getMessage());
        }
        if (echo != null) ok(java.util.Arrays.equals(line, echo), "connect-echo",
                             "sent=" + line.length + "B nonce=" + nonce);

        // --- 3. the socket's own addresses (lease + peer) ------------------
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress(dst, port), CONNECT_MS);
            String local = s.getLocalAddress().getHostAddress();
            String peer = s.getInetAddress().getHostAddress();
            boolean routable = !local.equals("0.0.0.0") && !local.startsWith("127.");
            ok(routable && peer.equals(host), "local-address",
               "local=" + local + ":" + s.getLocalPort() + " peer=" + peer);
        } catch (Throwable t) {
            ok(false, "local-address", t.getClass().getSimpleName() + ": " + t.getMessage());
        }

        // --- 4. bulk: 8192 B = 6 segments past TCP_MAX_PAYLOAD -------------
        byte[] bulk = pattern(8192, 7L);
        try {
            byte[] back = roundTrip(dst, port, bulk);
            ok(java.util.Arrays.equals(bulk, back), "bulk-8192",
               "bytes=" + back.length);
        } catch (Throwable t) {
            ok(false, "bulk-8192", t.getClass().getSimpleName() + ": " + t.getMessage());
        }

        // --- 5. reconnect: a second connection after the first closed ------
        try {
            byte[] again = roundTrip(dst, port, "AGAIN\n".getBytes("UTF-8"));
            ok(new String(again, "UTF-8").equals("AGAIN\n"), "reconnect",
               "second connection round-tripped");
        } catch (Throwable t) {
            ok(false, "reconnect", t.getClass().getSimpleName() + ": " + t.getMessage());
        }

        // --- 6. negative case: a closed port must fail, not hang -----------
        // The guest boots with nothing listening here; a RST from the host must
        // become ConnectException.  A timeout is reported as such, not as a
        // pass, because "no answer" is also what a broken stack looks like.
        String verdict;
        boolean refused;
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress(dst, port + 1), CONNECT_MS);
            verdict = "connected -- something IS listening on " + (port + 1);
            refused = false;
        } catch (ConnectException e) {
            verdict = "ConnectException: " + e.getMessage();
            refused = true;
        } catch (SocketTimeoutException e) {
            verdict = "timeout after " + CONNECT_MS + "ms (no RST seen)";
            refused = false;
        } catch (Throwable t) {
            verdict = t.getClass().getSimpleName() + ": " + t.getMessage();
            refused = false;
        }
        ok(refused, "refused-closed-port", verdict);

        summary();
    }

    static void summary() {
        System.out.println("TCP SUMMARY pass=" + pass + " fail=" + fail);
        System.out.println("SUMMARY pass=" + pass + " fail=" + fail + " skip=0");
        if (fail != 0) System.exit(1);
    }
}
