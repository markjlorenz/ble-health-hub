package com.ihealth.communication.ins;

import java.util.Arrays;

public class GenerateKap {
  static {
    // Matches the SDK's System.loadLibrary("iHealth") behavior.
    System.loadLibrary("iHealth");
  }

  public static native byte[] getKey(String selector);
  public static native byte[] getKa(String selector);

  private static String toHex(byte[] b) {
    if (b == null) return "<null>";
    StringBuilder sb = new StringBuilder(b.length * 2);
    for (byte x : b) sb.append(String.format("%02x", x));
    return sb.toString();
  }

  public static void main(String[] args) {
    String selector = args.length > 0 ? args[0] : "PO3";
    int times = args.length > 1 ? Integer.parseInt(args[1]) : 3;

    for (int i = 0; i < times; i++) {
      byte[] key = null;
      byte[] ka = null;
      Throwable keyErr = null;
      Throwable kaErr = null;
      try { key = getKey(selector); } catch (Throwable t) { keyErr = t; }
      try { ka = getKa(selector); } catch (Throwable t) { kaErr = t; }

      System.out.println("selector=" + selector + " run=" + (i + 1));
      if (keyErr != null) {
        System.out.println("  getKey ERROR: " + keyErr);
      } else {
        System.out.println("  getKey len=" + (key == null ? -1 : key.length) + " hex=" + toHex(key));
      }
      if (kaErr != null) {
        System.out.println("  getKa  ERROR: " + kaErr);
      } else {
        System.out.println("  getKa  len=" + (ka == null ? -1 : ka.length) + " hex=" + toHex(ka));
      }
    }
  }
}
