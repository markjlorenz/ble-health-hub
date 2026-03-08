import com.ihealth.communication.ins.GenerateKap;

public class Harness {
  public static void main(String[] args) {
    // Delegate to the packaged class so the JNI symbol name matches.
    GenerateKap.main(args);
  }
}
