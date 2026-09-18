import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
public class Callers extends GhidraScript {
  public void run() throws Exception {
    long target=0x10068200L;
    ReferenceManager rm=currentProgram.getReferenceManager(); FunctionManager fm=currentProgram.getFunctionManager();
    DecompInterface dec=new DecompInterface(); dec.openProgram(currentProgram); dec.setSimplificationStyle("decompile");
    java.util.HashSet<Address> done=new java.util.HashSet<>();
    for(ReferenceIterator ri=rm.getReferencesTo(toAddr(target)); ri.hasNext();){
      Function c=fm.getFunctionContaining(ri.next().getFromAddress());
      if(c==null||!done.add(c.getEntryPoint())) continue;
      println("\n==== "+c.getName()+" @ "+c.getEntryPoint()+" sig="+c.getSignature()+" ====");
      DecompileResults r=dec.decompileFunction(c,120,monitor);
      if(r!=null&&r.decompileCompleted()){ String s=r.getDecompiledFunction().getC(); println(s.length()>5000?s.substring(0,5000):s); }
    }
  }
}
