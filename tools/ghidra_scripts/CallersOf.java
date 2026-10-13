// Headless helper: list callers of the functions at the given addresses (script args, hex)
// and decompile each calling function once.
// analyzeHeadless <proj> <name> -process <prog> -noanalysis -scriptPath tools/ghidra_scripts \
//   -postScript CallersOf.java 0x56d0
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.util.LinkedHashSet;
import java.util.Set;

public class CallersOf extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        Set<Function> callers = new LinkedHashSet<>();
        for (String arg : getScriptArgs()) {
            Address target = toAddr(Long.decode(arg));
            for (Reference ref : getReferencesTo(target)) {
                Function f = getFunctionContaining(ref.getFromAddress());
                println("=== ref " + arg + " from " + ref.getFromAddress() + " (" + ref.getReferenceType() + ") in "
                        + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                if (f != null) callers.add(f);
            }
        }
        for (Function f : callers) {
            DecompileResults r = ifc.decompileFunction(f, 60, monitor);
            println("=== " + f.getName() + " @ " + f.getEntryPoint());
            println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : r.getErrorMessage());
        }
    }
}
