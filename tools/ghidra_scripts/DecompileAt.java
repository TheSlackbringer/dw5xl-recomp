// Headless helper: decompile the functions containing the given addresses (script args, hex).
// analyzeHeadless <proj> <name> -process <prog> -noanalysis -scriptPath tools/ghidra_scripts \
//   -postScript DecompileAt.java 0x6d9c 0x1234
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            Address addr = toAddr(Long.decode(arg));
            Function f = getFunctionContaining(addr);
            if (f == null) {
                println("=== " + arg + ": no function");
                continue;
            }
            DecompileResults r = ifc.decompileFunction(f, 60, monitor);
            println("=== " + arg + " in " + f.getName() + " @ " + f.getEntryPoint());
            println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : r.getErrorMessage());
        }
    }
}
