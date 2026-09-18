import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
public class FindStr extends GhidraScript {
  public void run() throws Exception {
    String needle = "InternalSave( PNGIO";
    Listing lst = currentProgram.getListing();
    DataIterator di = lst.getDefinedData(true);
    Address found=null;
    while (di.hasNext()){ Data d=di.next(); Object v=d.getValue();
      if (v!=null && v.toString().contains(needle)){ found=d.getAddress(); println("string @ "+found+" : "+v.toString().substring(0,Math.min(60,v.toString().length()))); break; } }
    if(found==null){ println("string no encontrado como Data; buscando bytes..."); 
      Address a=currentProgram.getMinAddress();
      found=find(a, needle.getBytes());
      println("bytes @ "+found);
    }
    if(found==null) return;
    // xrefs a ese string
    ReferenceManager rm=currentProgram.getReferenceManager();
    ReferenceIterator ri=rm.getReferencesTo(found);
    DecompInterface dec=new DecompInterface(); dec.openProgram(currentProgram); dec.setSimplificationStyle("decompile");
    java.util.HashSet<Address> done=new java.util.HashSet<>();
    int n=0;
    while(ri.hasNext() && n<2){ Reference r=ri.next(); Address from=r.getFromAddress();
      Function f=currentProgram.getFunctionManager().getFunctionContaining(from);
      if(f==null||!done.add(f.getEntryPoint())) continue;
      println("\n==== xref desde "+from+" en "+f.getName()+" @ "+f.getEntryPoint()+" ====");
      DecompileResults res=dec.decompileFunction(f,120,monitor);
      if(res!=null&&res.decompileCompleted()) println(res.getDecompiledFunction().getC());
      n++;
    }
  }
}
