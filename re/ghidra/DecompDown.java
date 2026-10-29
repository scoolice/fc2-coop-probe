// Decompiles the function at each given address (creating it if Ghidra has none there), then its callees down to a depth.
// Headless: -postScript DecompDown.java <out file> <depth> <address> [<address> ...]   (addresses in hex, e.g. 109f9080)
// @category coop

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompDown extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;
    private final Set<Function> done = new LinkedHashSet<>();

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        int depth = Integer.parseInt(args[1]);
        try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
            out = w;
            dec = new DecompInterface();
            dec.openProgram(currentProgram);
            for (int i = 2; i < args.length; i++) {
                Address a = toAddr(args[i]);
                Function f = getFunctionAt(a);
                if (f == null) f = createFunction(a, null);
                if (f == null) f = getFunctionContaining(a);
                if (f == null) { out.println("=== no function at " + a); continue; }
                walk(f, depth, "requested " + args[i]);
            }
        }
    }

    private void walk(Function f, int depth, String why) {
        if (!done.add(f) || monitor.isCancelled()) return;
        DecompileResults r = dec.decompileFunction(f, 60, monitor);
        out.println("=== " + f.getName() + " @ " + f.getEntryPoint() + " (" + why + ")");
        out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "  decompile failed: " + r.getErrorMessage());
        if (depth <= 0) return;
        for (Function c : f.getCalledFunctions(monitor)) walk(c, depth - 1, "called by " + f.getName());
    }
}
