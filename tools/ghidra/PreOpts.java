// Reduce analysis cost for a large PE in a small container
import ghidra.app.script.GhidraScript;
import java.util.Map;
public class PreOpts extends GhidraScript {
  public void run() throws Exception {
    Map<String,String> opts = getCurrentAnalysisOptionsAndValues(currentProgram);
    String[] off = {"Decompiler Parameter ID", "Aggressive Instruction Finder", "Condense Filler Bytes", "Embedded Media", "PDB Universal", "PDB MSDIA", "Scalar Operand References", "Variadic Function Signature Override"};
    for (String k : off) if (opts.containsKey(k)) { setAnalysisOption(currentProgram, k, "false"); println("disabled " + k); }
  }
}
