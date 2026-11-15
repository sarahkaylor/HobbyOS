package demo;

/**
 * One class, enough surface that {@code javadoc} has to emit every section it
 * is capable of: class doc, field doc, constructor doc, method doc, param,
 * return, throws, {@code @see} and {@code @since}.
 *
 * @since 27
 */
public final class Welcome {

    /** The greeting prefix. */
    public static final String PREFIX = "hello, ";

    /** Who the greeting is for. */
    private final String subject;

    /**
     * Builds a greeter.
     *
     * @param subject the name to greet, never {@code null}
     */
    public Welcome(String subject) {
        this.subject = subject;
    }

    /**
     * Greets the subject.
     *
     * @param times how many times to repeat the greeting
     * @return the greeting, repeated
     * @throws IllegalArgumentException if {@code times} is negative
     * @see #PREFIX
     */
    public String greet(int times) {
        if (times < 0) {
            throw new IllegalArgumentException("times < 0: " + times);
        }
        return (PREFIX + subject + "\n").repeat(times).stripTrailing();
    }

    /**
     * Entry point, so the class is also runnable.
     *
     * @param args ignored
     */
    public static void main(String[] args) {
        System.out.println(new Welcome("hobbyos").greet(1));
    }
}
