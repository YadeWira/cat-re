// @category Decompile
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import java.util.LinkedHashSet;
import java.util.Set;

public class DecompBatch extends GhidraScript {
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        decomp.setSimplificationStyle("decompile");
        long[] targets = {
            0x1000edb4L, 0x1000f2fcL,
        };
        Set<Address> seen = new LinkedHashSet<>();
        for (long rva : targets) {
            Address addr = toAddr(rva);
            Function f = fm.getFunctionContaining(addr);
            if (f == null) { println("// NO FUNCTION containing " + Long.toHexString(rva)); continue; }
            if (!seen.add(f.getEntryPoint())) continue;
            println("\n//===== 0x" + Long.toHexString(rva) + " -> " + f.getName() + " @ " + f.getEntryPoint());
            println("// sig: " + f.getSignature());
            DecompileResults res = decomp.decompileFunction(f, 90, monitor);
            if (res != null && res.decompileCompleted()) println(res.getDecompiledFunction().getC());
            else println("// FAIL: " + (res==null?"null":res.getErrorMessage()));
        }
        println("\n// DONE");
    }
}
