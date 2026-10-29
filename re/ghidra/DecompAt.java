// Decompiles the function containing each given address, then the functions calling it (one level).
// Headless: -postScript DecompAt.java <out file> <address> [<address> ...]   (addresses in hex, e.g. 109fcf02)
// @category coop

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DecompAt extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;
    private final Set<Function> done = new LinkedHashSet<>();

    private void decompile(Function f, String why) {
        if (f == null || !done.add(f)) return;
        DecompileResults res = dec.decompileFunction(f, 60, monitor);
        out.println("=== " + f.getName() + " @ " + f.getEntryPoint() + " (" + why + ")");
        out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "(decompile failed: " + res.getErrorMessage() + ")");
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
            out = w;
            dec = new DecompInterface();
            dec.openProgram(currentProgram);
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(Long.parseLong(args[i], 16));
                Function f = getFunctionContaining(a);
                out.println("##### address " + a + (f == null ? " (no function)" : " in " + f.getName() + " @ " + f.getEntryPoint()));
                out.println("# instruction: " + getInstructionAt(a));
                if (f == null) continue;
                decompile(f, "contains " + a);
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    Function caller = getFunctionContaining(r.getFromAddress());
                    out.println("#   called from " + r.getFromAddress() + (caller == null ? "" : " in " + caller.getName()));
                    decompile(caller, "calls " + f.getName());
                }
            }
            dec.dispose();
        }
    }
}
