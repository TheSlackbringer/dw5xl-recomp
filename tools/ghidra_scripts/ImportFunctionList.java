// Creates functions from a recompiler function list (Name,Start,End,Size CSV), e.g.
// recomp/dw5xl/overlays/ovl00/ghidra_functions.code.csv for a raw-imported overlay.
// Disassembles each start, creates the function if missing, and keeps existing names
// unless the current name is a default FUN_ name.
//@category DW5XL

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ImportFunctionList extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File csv = askFile("Function list CSV (Name,Start,End,Size)", "Import");
        int created = 0, existing = 0, failed = 0;
        try (BufferedReader in = new BufferedReader(new FileReader(csv))) {
            String line = in.readLine(); // header
            while ((line = in.readLine()) != null && !monitor.isCancelled()) {
                String[] f = line.split(",");
                if (f.length < 2)
                    continue;
                Address start = toAddr(f[1].trim());
                if (start == null || currentProgram.getMemory().getBlock(start) == null) {
                    failed++;
                    continue;
                }
                Function fn = getFunctionAt(start);
                if (fn == null) {
                    disassemble(start);
                    fn = createFunction(start, f[0].trim());
                    if (fn == null) {
                        failed++;
                        continue;
                    }
                    created++;
                } else {
                    existing++;
                    if (fn.getName().startsWith("FUN_") && !f[0].trim().startsWith("FUN_"))
                        fn.setName(f[0].trim(), SourceType.IMPORTED);
                }
            }
        }
        println("ImportFunctionList: created " + created + ", already present " + existing + ", failed " + failed);
    }
}
