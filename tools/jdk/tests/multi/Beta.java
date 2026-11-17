package pkg;

public class Beta {
  static final int[] SEED = {3, 1, 4, 1, 5};

  public static String tag(int i) {
    return "b" + (SEED[i % SEED.length] + i);
  }
}
