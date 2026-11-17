/*
 * tools/jdk/tests/Hello.java -- the smallest SDK acceptance program.
 *
 * Its job is to be compiled BY THE GUEST'S javac (wave 1) and then run BY THE
 * GUEST'S java (wave 2), so it must not use anything exotic: if this fails,
 * the failure is the toolchain and not one of the library subsystems Verify
 * exercises.
 */
public class Hello {
    public static void main(String[] args) {
        System.out.println("HELLO-JAVA: Hello from OpenJDK on HobbyOS");
        System.out.println("HELLO-JAVA: args=" + args.length);
        for (String a : args) System.out.println("HELLO-JAVA: arg=" + a);
        System.out.println("HELLO-JAVA: java.version=" + System.getProperty("java.version"));
        System.out.println("HELLO-JAVA: os.name=" + System.getProperty("os.name"));
        System.out.println("HELLO-JAVA: os.arch=" + System.getProperty("os.arch"));
        System.out.println("HELLO-JAVA: user.dir=" + System.getProperty("user.dir"));
        System.out.println("HELLO-JAVA: file.separator=" + System.getProperty("file.separator"));
        System.out.println("HELLO-JAVA: DONE");
    }
}
