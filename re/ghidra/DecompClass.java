// For each class name: finds the function that registers the name (the class's type getter), the vtables
// holding a pointer to that getter, and decompiles every slot of those vtables up to the first non-code pointer.
// Headless: -postScript DecompClass.java <out file> <class name> [<class name> ...]
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
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class DecompClass extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;
    private final Set<Function> done = new LinkedHashSet<>();

    private boolean isCode(Address a) {
        MemoryBlock b = getMemoryBlock(a);
        return b != null && b.isExecute();
    }

    private void decompile(Function f, String why) {
        if (f == null || !done.add(f)) { if (f != null) out.println("=== " + f.getName() + " (" + why + ", shown above)"); return; }
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
                String want = args[i];
                out.println("########## class " + want);
                DataIterator it = currentProgram.getListing().getDefinedData(true);
                while (it.hasNext() && !monitor.isCancelled()) {
                    Data d = it.next();
                    if (!want.equals(d.getValue())) continue;
                    for (Reference r : getReferencesTo(d.getAddress())) {
                        Function getter = getFunctionContaining(r.getFromAddress());
                        if (getter == null) continue;
                        out.println("# name used in " + getter.getName() + " @ " + getter.getEntryPoint());
                        for (Reference vr : getReferencesTo(getter.getEntryPoint())) {
                            Address slot = vr.getFromAddress();
                            if (isCode(slot)) continue;
                            // walk back to the vtable start: consecutive code pointers
                            Address start = slot;
                            while (true) {
                                Address prev = start.subtract(4);
                                Address t = toAddr(getInt(prev) & 0xFFFFFFFFL);
                                if (!isCode(t) || getReferencesTo(start).length > 0 && !start.equals(slot)) break;
                                start = prev;
                            }
                            out.println("## vtable from " + start + " (getter at " + slot + ")");
                            for (int s = 0; s < 80; s++) {
                                Address a = start.add(4L * s);
                                Address t = toAddr(getInt(a) & 0xFFFFFFFFL);
                                if (!isCode(t)) break;
                                if (s > 0 && getReferencesTo(a).length > 0) break;
                                Function f = getFunctionAt(t);
                                if (f == null) f = createFunction(t, null);
                                out.println("# slot " + s + " (+0x" + Integer.toHexString(4 * s) + ") -> " + t);
                                decompile(f, want + " slot " + s);
                            }
                        }
                    }
                }
            }
            dec.dispose();
        }
    }
}
