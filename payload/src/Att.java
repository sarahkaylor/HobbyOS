import com.sun.tools.attach.VirtualMachine;
import com.sun.tools.attach.VirtualMachineDescriptor;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/**
 * One process, no sidekick: attach to <em>ourselves</em> with the same
 * com.sun.tools.attach machinery jcmd/jstack/jmap use, so the attach path can be
 * exercised in a boot that runs exactly one program.
 *
 * Prints one line per step so a failure is attributable to a syscall rather than
 * to "attach is broken".
 */
public class Att {

    static void step(String s) {
        System.out.println("[ATT] " + s);
        System.out.flush();
    }

    static void cause(Throwable t, String indent) {
        for (Throwable c = t; c != null; c = c.getCause()) {
            System.out.println("[ATT] " + indent + c.getClass().getName() + ": " + c.getMessage());
            for (StackTraceElement e : c.getStackTrace()) {
                String s = e.toString();
                // only the frames that point at a syscall boundary
                if (s.contains("sun.nio") || s.contains("java.io") || s.contains("attach")
                        || s.contains("ProcessHandle") || s.contains("Native")) {
                    System.out.println("[ATT] " + indent + "  at " + s);
                }
            }
            indent = indent + "  ";
        }
        System.out.flush();
    }

    public static void main(String[] args) {
        long pid = ProcessHandle.current().pid();
        step("pid=" + pid);
        step("java.io.tmpdir=" + System.getProperty("java.io.tmpdir"));
        step("os.name=" + System.getProperty("os.name") + " ver=" + System.getProperty("os.version"));

        for (String p : new String[]{"/proc", "/proc/self", "/proc/self/stat", "/proc/self/task",
                "/tmp", "/tmp/.java_pid" + pid}) {
            File f = new File(p);
            step("exists(" + p + ")=" + f.exists() + (f.exists() ? (f.isDirectory() ? " dir" : " file") : ""));
        }

        try {
            Path maps = Path.of("/proc/self/maps");
            if (Files.isReadable(maps)) {
                List<String> l = Files.readAllLines(maps);
                step("/proc/self/maps readable, " + l.size() + " lines, first=" + (l.isEmpty() ? "-" : l.get(0)));
            } else {
                step("/proc/self/maps not readable");
            }
        } catch (Throwable t) {
            step("/proc/self/maps threw");
            cause(t, "  ");
        }

        try {
            List<VirtualMachineDescriptor> vms = VirtualMachine.list();
            step("VirtualMachine.list()=" + vms.size() + " " + vms);
        } catch (Throwable t) {
            step("VirtualMachine.list() FAILED");
            cause(t, "  ");
        }

        VirtualMachine vm = null;
        try {
            step("attaching to self ...");
            vm = VirtualMachine.attach(String.valueOf(pid));
            step("attach OK, provider=" + vm.provider().type());
            step("systemProperties(tool)=" + vm.getSystemProperties().getProperty("java.vm.name"));

            step("loadAgent(management-agent) ...");
            String agent = System.getProperty("java.home") + "/lib/libmanagement_agent.so";
            vm.loadAgent(agent);
            step("loadAgent OK");
        } catch (Throwable t) {
            step("ATTACH FAILED");
            cause(t, "  ");
        } finally {
            if (vm != null) {
                try {
                    vm.detach();
                    step("detach OK");
                } catch (Throwable t) {
                    step("detach FAILED");
                    cause(t, "  ");
                }
            }
        }

        step("bye");
    }
}
