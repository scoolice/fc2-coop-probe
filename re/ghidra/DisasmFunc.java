// Prints the disassembly of the function at each given address (creating it if Ghidra has none there).
// Headless: -postScript DisasmFunc.java <out file> <address> [<address> ...]   (addresses in hex, e.g. 109f9310)
// @category coop

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class DisasmFunc extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(args[i]);
                Function f = getFunctionAt(a);
                if (f == null) f = createFunction(a, null);
                if (f == null) f = getFunctionContaining(a);
                if (f == null) { out.println("=== no function at " + a); continue; }
                out.println("=== " + f.getName() + " @ " + f.getEntryPoint());
                InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (it.hasNext()) {
                    Instruction ins = it.next();
                    out.println("  " + ins.getAddress() + "  " + ins);
                }
            }
        }
    }
}
