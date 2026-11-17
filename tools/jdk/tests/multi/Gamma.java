package pkg;

public sealed interface Gamma permits Gamma.Impl {
  static String describe() { return new Impl().name(); }

  final class Impl implements Gamma {
    String name() { return "gamma/" + Integer.toHexString(hashCode() & 0xFF); }
  }
}
