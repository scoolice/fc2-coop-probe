// Decompiles every function that references one of the given strings (exact match, ASCII or UTF-16).
// Headless: -postScript DecompStringRefs.java <out file> <string> [<string> ...]
// @category coop

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DecompStringRefs extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            DecompInterface dec = new DecompInterface();
            dec.openProgram(currentProgram);
            Set<Function> done = new LinkedHashSet<>();
            for (int i = 1; i < args.length; i++) {
                String want = args[i];
                out.println("##### string \"" + want + "\"");
                DataIterator it = currentProgram.getListing().getDefinedData(true);
                while (it.hasNext() && !monitor.isCancelled()) {
                    Data d = it.next();
                    Object v = d.getValue();
                    if (!(v instanceof String) || !want.equals(v)) continue;
                    out.println("# at " + d.getAddress());
                    for (Reference r : getReferencesTo(d.getAddress())) {
                        Address from = r.getFromAddress();
                        Function f = getFunctionContaining(from);
                        out.println("#   ref from " + from + (f == null ? " (no function)" : " in " + f.getName() + " @ " + f.getEntryPoint()));
                        if (f == null || !done.add(f)) continue;
                        DecompileResults res = dec.decompileFunction(f, 60, monitor);
                        out.println("=== " + f.getName() + " @ " + f.getEntryPoint());
                        out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "(decompile failed: " + res.getErrorMessage() + ")");
                    }
                }
            }
            dec.dispose();
        }
    }
}
