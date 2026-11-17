
import java.awt.*;
import java.awt.font.*;
import java.awt.image.*;
import java.util.*;

/** How far java.desktop gets with no display: AWT primitives, fonts and text,
 *  Graphics2D rendering into a BufferedImage, image pipelines, then Swing
 *  component creation, layout, and events on the EDT.
 *
 *  Deliberately reports each capability separately and keeps going: the question
 *  is "which part breaks", not "does it all work".  Any exception is caught and
 *  printed with its class so a missing font or a missing native library is
 *  distinguishable from a hang.
 */
public class UiHeadless {
  static int pass = 0, fail = 0;
  static void ok(String what, boolean good, String detail) {
    if (good) pass++; else fail++;
    System.out.println((good ? "UI PASS " : "UI FAIL ") + what + (detail == null ? "" : " " + detail));
  }
  static String ex(Throwable t) { return t.getClass().getName() + ": " + String.valueOf(t.getMessage()); }

  public static void main(String[] a) {
    System.out.println("UI headless=" + GraphicsEnvironment.isHeadless());

    // 1. headless toolkit + screen metrics
    try {
      Toolkit tk = Toolkit.getDefaultToolkit();
      /* getScreenSize() throws HeadlessException BY SPEC when headless, so asking
         for it here measured the spec and not the port.  Record it as the spec
         says, and report the screen size only when a display exists. */
      String screen = "n/a-headless";
      if (!GraphicsEnvironment.isHeadless()) {
        Dimension d = tk.getScreenSize();
        screen = d.width + "x" + d.height;
      }
      ok("toolkit", tk != null, "class=" + tk.getClass().getName() + " screen=" + screen);
    } catch (Throwable t) { ok("toolkit", false, ex(t)); }

    // 2. fonts available at all
    try {
      String[] fams = GraphicsEnvironment.getLocalGraphicsEnvironment().getAvailableFontFamilyNames();
      ok("font-families", fams.length > 0, "count=" + fams.length +
         (fams.length > 0 ? " first=" + fams[0] : ""));
      Font f = new Font("Dialog", Font.PLAIN, 12);
      ok("font-dialog", f != null && f.getSize() == 12, "name=" + (f == null ? "null" : f.getFamily()));
    } catch (Throwable t) { ok("font-families", false, ex(t)); }

    // 3. text metrics + drawing into an image
    try {
      BufferedImage img = new BufferedImage(200, 60, BufferedImage.TYPE_INT_RGB);
      Graphics2D g = img.createGraphics();
      g.setColor(Color.WHITE); g.fillRect(0, 0, 200, 60);
      g.setColor(Color.BLACK);
      Font f = new Font("Dialog", Font.PLAIN, 14);
      FontMetrics fm = g.getFontMetrics(f);
      int w = fm.stringWidth("HobbyOS");
      g.drawString("HobbyOS", 10, 30);
      g.dispose();
      int ink = 0;
      for (int y = 0; y < 60; y++) for (int x = 0; x < 200; x++) if ((img.getRGB(x, y) & 0xFFFFFF) != 0xFFFFFF) ink++;
      ok("text-metrics", w > 0, "stringWidth=" + w + " ascent=" + fm.getAscent());
      ok("text-render", ink > 0, "non-white-pixels=" + ink);
    } catch (Throwable t) { ok("text-render", false, ex(t)); }

    // 4. 2D geometry + antialiasing into an image
    try {
      BufferedImage img = new BufferedImage(100, 100, BufferedImage.TYPE_INT_ARGB);
      Graphics2D g = img.createGraphics();
      g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
      g.setColor(new Color(255, 0, 0, 128));
      g.fill(new java.awt.geom.Ellipse2D.Double(10, 10, 80, 80));
      g.setStroke(new BasicStroke(3f));
      g.draw(new java.awt.geom.Line2D.Double(0, 0, 100, 100));
      g.dispose();
      int red = 0;
      for (int y = 0; y < 100; y++) for (int x = 0; x < 100; x++) if ((img.getRGB(x, y) >>> 24) != 0) red++;
      ok("g2d-shapes", red > 1000, "painted-pixels=" + red);
    } catch (Throwable t) { ok("g2d-shapes", false, ex(t)); }

    // 5. image pipeline: scaling, colour model conversion
    try {
      BufferedImage src = new BufferedImage(64, 64, BufferedImage.TYPE_INT_RGB);
      Graphics2D g = src.createGraphics(); g.setColor(Color.BLUE); g.fillRect(0, 0, 64, 64); g.dispose();
      BufferedImage dst = new BufferedImage(32, 32, BufferedImage.TYPE_INT_ARGB);
      Graphics2D g2 = dst.createGraphics();
      g2.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BILINEAR);
      g2.drawImage(src, 0, 0, 32, 32, null); g2.dispose();
      ok("image-scale", (dst.getRGB(16, 16) & 0xFFFFFF) == 0x0000FF, "px=" + Integer.toHexString(dst.getRGB(16, 16)));
    } catch (Throwable t) { ok("image-scale", false, ex(t)); }

    // 6. Swing: create components, set a model, run a layout pass
    try {
      javax.swing.JPanel root = new javax.swing.JPanel(new java.awt.BorderLayout());
      root.add(new javax.swing.JLabel("hello"), java.awt.BorderLayout.NORTH);
      root.add(new javax.swing.JTextField("abc"), java.awt.BorderLayout.CENTER);
      javax.swing.JButton b = new javax.swing.JButton("go");
      root.add(b, java.awt.BorderLayout.SOUTH);
      root.setSize(300, 200);
      root.doLayout();
      java.awt.Dimension pref = root.getPreferredSize();
      ok("swing-components", root.getComponentCount() == 3, "children=" + root.getComponentCount()
         + " pref=" + pref.width + "x" + pref.height + " btnText=" + b.getText());
    } catch (Throwable t) { ok("swing-components", false, ex(t)); }

    // 7. Swing look and feel install
    try {
      javax.swing.UIManager.setLookAndFeel(javax.swing.UIManager.getSystemLookAndFeelClassName());
      ok("swing-laf", javax.swing.UIManager.getLookAndFeel() != null, "laf=" + javax.swing.UIManager.getLookAndFeel().getClass().getName());
    } catch (Throwable t) { ok("swing-laf", false, ex(t)); }

    // 8. EDT exists and runs a task
    try {
      final java.util.concurrent.CountDownLatch l = new java.util.concurrent.CountDownLatch(1);
      final boolean[] onEdt = {false};
      javax.swing.SwingUtilities.invokeLater(() -> { onEdt[0] = javax.swing.SwingUtilities.isEventDispatchThread(); l.countDown(); });
      boolean ran = l.await(60, java.util.concurrent.TimeUnit.SECONDS);
      ok("edt", ran && onEdt[0], "ran=" + ran + " onEdt=" + onEdt[0]);
    } catch (Throwable t) { ok("edt", false, ex(t)); }

    // 9. offscreen painting of a component hierarchy (needs no window)
    try {
      javax.swing.JPanel p = new javax.swing.JPanel();
      p.setBackground(Color.RED); p.setSize(50, 50);
      BufferedImage img = new BufferedImage(50, 50, BufferedImage.TYPE_INT_RGB);
      Graphics2D g = img.createGraphics();
      p.paint(g);
      g.dispose();
      ok("swing-offscreen-paint", (img.getRGB(25, 25) & 0xFFFFFF) == 0xFF0000, "px=" + Integer.toHexString(img.getRGB(25, 25)));
    } catch (Throwable t) { ok("swing-offscreen-paint", false, ex(t)); }

    System.out.println("UI SUMMARY pass=" + pass + " fail=" + fail);
    System.out.println("UI VERDICT " + (fail == 0 ? "PASS" : "PARTIAL"));
  }
}
