package pkg;

import java.util.ArrayList;
import java.util.List;
import java.util.stream.Collectors;

/** Multi-source javac case: Alpha refers to Beta, so one javac invocation has
 *  to resolve three compilation units and emit three class files. */
public class Alpha {
  public static String run() {
    List<String> xs = new ArrayList<>();
    for (int i = 0; i < 5; i++) xs.add(Beta.tag(i));
    String joined = xs.stream().map(s -> s.toUpperCase()).collect(Collectors.joining(","));
    return joined + " | " + Gamma.describe();
  }

  public static void main(String[] a) {
    System.out.println("MULTI-JAVAC: " + run());
  }
}
