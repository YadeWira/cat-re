import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
public class FindDec extends GhidraScript {
  public void run() throws Exception {
    // localizar string de error de zlib inflate, su xref = inflate(); callers = decompress wrapper
    String needle="incorrect header check";
    Listing lst=currentProgram.getListing(); DataIterator di=lst.getDefinedData(true);
    Address sa=null;
    while(di.hasNext()){ Data d=di.next(); Object v=d.getValue();
      if(v!=null && v.toString().contains(needle)){ sa=d.getAddress(); break; } }
    if(sa==null){ println("string no encontrada"); return; }
    println("zlib err string @ "+sa);
    ReferenceManager rm=currentProgram.getReferenceManager(); FunctionManager fm=currentProgram.getFunctionManager();
    // funcion que usa la string = inflate
    java.util.HashSet<Address> inflate=new java.util.HashSet<>();
    for(ReferenceIterator ri=rm.getReferencesTo(sa); ri.hasNext();){ Function f=fm.getFunctionContaining(ri.next().getFromAddress()); if(f!=null) inflate.add(f.getEntryPoint()); }
    println("inflate candidatas: "+inflate);
    // callers de inflate
    DecompInterface dec=new DecompInterface(); dec.openProgram(currentProgram); dec.setSimplificationStyle("decompile");
    java.util.HashSet<Address> done=new java.util.HashSet<>(); int n=0;
    for(Address ia: inflate){ Function inf=fm.getFunctionAt(ia);
      for(ReferenceIterator ri=rm.getReferencesTo(ia); ri.hasNext();){
        Function c=fm.getFunctionContaining(ri.next().getFromAddress());
        if(c==null||!done.add(c.getEntryPoint())||n>=3) continue;
        // heuristica: decompilar callers cuya func sea mediana (wrapper), imprimir si menciona el header
        DecompileResults r=dec.decompileFunction(c,90,monitor);
        if(r!=null&&r.decompileCompleted()){ String code=r.getDecompiledFunction().getC();
          if(code.contains("0x132")||code.contains("0x198")||code.length()<4000){ // candidatos
            println("\n==== caller "+c.getName()+" @ "+c.getEntryPoint()+" (len "+code.length()+") ====");
            println(code.length()>3500?code.substring(0,3500):code); n++; } } }
    }
  }
}
