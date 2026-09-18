// @category Decompile
// Decompila las funciones que referencian strings de selección de codec/engine.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.util.*;

public class FindPick extends GhidraScript {
  public void run() throws Exception {
    String[] needles = getScriptArgs().length > 0 ? getScriptArgs()
        : new String[]{"EngineID", "QuikCAT\\CODEC", "ICPEngine"};
    Listing lst = currentProgram.getListing();
    DecompInterface dec = new DecompInterface();
    dec.openProgram(currentProgram);
    dec.setSimplificationStyle("decompile");
    ReferenceManager rm = currentProgram.getReferenceManager();
    Set<Address> done = new HashSet<>();

    for (String needle : needles) {
      println("\n########## buscando: " + needle);
      DataIterator di = lst.getDefinedData(true);
      List<Address> hits = new ArrayList<>();
      while (di.hasNext()) {
        Data d = di.next();
        Object v = d.getValue();
        if (v != null && v.toString().contains(needle)) hits.add(d.getAddress());
      }
      println("  strings encontrados: " + hits.size());
      for (Address s : hits) {
        ReferenceIterator ri = rm.getReferencesTo(s);
        while (ri.hasNext()) {
          Function f = currentProgram.getFunctionManager()
              .getFunctionContaining(ri.next().getFromAddress());
          if (f == null || !done.add(f.getEntryPoint())) continue;
          println("\n==== " + f.getName() + " @ " + f.getEntryPoint() + " ====");
          DecompileResults res = dec.decompileFunction(f, 180, monitor);
          if (res != null && res.decompileCompleted())
            println(res.getDecompiledFunction().getC());
        }
      }
    }
  }
}
