// Decompile listed addresses on a non-analyzed import: creates functions on demand.
//@category EU4
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class DecompTargets extends GhidraScript {
  Function ensure(Address a) {
    Function f = getFunctionAt(a);
    if (f != null) return f;
    DisassembleCommand dc = new DisassembleCommand(a, null, true);
    dc.applyTo(currentProgram, monitor);
    CreateFunctionCmd cf = new CreateFunctionCmd(a);
    cf.applyTo(currentProgram, monitor);
    return getFunctionAt(a);
  }
  public void run() throws Exception {
    String[] args = getScriptArgs();
    List<String> lines = Files.readAllLines(Paths.get(args[0]));
    File outDir = new File(args[1]); outDir.mkdirs();
    // pre-create functions for all known entries (cheap names for callees), once
    File marker = new File(args[1], ".funcs_created");
    if (args.length > 2 && !marker.exists()) {
      int n = 0;
      for (String l : Files.readAllLines(Paths.get(args[2]))) {
        String[] p = l.split("\t");
        if (p[0].equals("entry")) continue;
        try { Address a = toAddr(Long.parseUnsignedLong(p[0], 16)); if (getFunctionAt(a) == null && getInstructionAt(a) == null) { if (ensure(a) != null) n++; } } catch (Exception e) {}
        if (monitor.isCancelled()) break;
      }
      println("created functions: " + n);
      new FileOutputStream(marker).close();
    }
    DecompInterface d = new DecompInterface();
    DecompileOptions o = new DecompileOptions(); o.grabFromProgram(currentProgram);
    d.setOptions(o); d.toggleCCode(true); d.openProgram(currentProgram);
    for (String l : lines) {
      l = l.trim(); if (l.isEmpty()) continue;
      Address a = toAddr(Long.parseUnsignedLong(l, 16));
      Function f = ensure(a);
      String text;
      if (f == null) text = "/* no function at " + l + " */";
      else {
        DecompileResults r = d.decompileFunction(f, 120, monitor);
        text = r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : "/* failed: " + r.getErrorMessage() + " */";
      }
      try (PrintWriter w = new PrintWriter(new File(outDir, l + ".c"), "UTF-8")) { w.println(text); }
    }
    d.dispose();
  }
}
