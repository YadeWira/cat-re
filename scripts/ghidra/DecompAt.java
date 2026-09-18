// @category Decompile
// Decompila funciones por dirección (args) y lista quién las llama.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class DecompAt extends GhidraScript {
  public void run() throws Exception {
    DecompInterface dec = new DecompInterface();
    dec.openProgram(currentProgram);
    dec.setSimplificationStyle("decompile");
    FunctionManager fm = currentProgram.getFunctionManager();
    for (String a : getScriptArgs()) {
      Address addr = currentProgram.getAddressFactory().getAddress(a);
      Function f = fm.getFunctionContaining(addr);
      if (f == null) { println("sin función en " + a); continue; }
      println("\n==== " + f.getName() + " @ " + f.getEntryPoint() + " ====");
      DecompileResults r = dec.decompileFunction(f, 180, monitor);
      if (r != null && r.decompileCompleted()) println(r.getDecompiledFunction().getC());
      println("---- llamada desde: ----");
      for (Reference ref : getReferencesTo(f.getEntryPoint())) {
        Function c = fm.getFunctionContaining(ref.getFromAddress());
        if (c != null) println("   " + c.getName() + " @ " + c.getEntryPoint());
      }
    }
  }
}
