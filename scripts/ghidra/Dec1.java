import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
public class Dec1 extends GhidraScript {
  public void run() throws Exception {
    DecompInterface dec=new DecompInterface(); dec.openProgram(currentProgram); dec.setSimplificationStyle("decompile");
    Function f=currentProgram.getFunctionManager().getFunctionContaining(toAddr(0x1002a300L));
    println("// "+f.getName()+" @ "+f.getEntryPoint()+"  "+f.getSignature());
    DecompileResults r=dec.decompileFunction(f,180,monitor);
    if(r!=null&&r.decompileCompleted()) println(r.getDecompiledFunction().getC());
    else println("FAIL");
  }
}
