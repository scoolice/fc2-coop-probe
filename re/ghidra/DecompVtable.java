// Decompiles code at addresses (creating a function there if Ghidra has none) and the targets of a vtable.
// Headless: -postScript DecompVtable.java <out file> <vtable hex> <slots> [<code hex> ...]
// @category coop

import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompVtable extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;

    private void decompileAt(Address a, String why) throws Exception {
        Function f = getFunctionAt(a);
        if (f == null) f = getFunctionContaining(a);
        if (f == null) f = createFunction(a, null);
        out.println("=== " + (f == null ? "(no function)" : f.getName() + " @ " + f.getEntryPoint()) + " (" + why + ")");
        if (f == null) return;
        DecompileResults res = dec.decompileFunction(f, 60, monitor);
        out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "(decompile failed: " + res.getErrorMessage() + ")");
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
            out = w;
            dec = new DecompInterface();
            dec.openProgram(currentProgram);
            Address vt = toAddr(Long.parseLong(args[1], 16));
            int slots = Integer.parseInt(args[2]);
            for (int i = 0; i < slots; i++) {
                Address slot = vt.add(4L * i);
                Address target = toAddr(getInt(slot) & 0xFFFFFFFFL);
                out.println("##### vtable " + vt + " slot " + i + " (+0x" + Integer.toHexString(4 * i) + ") -> " + target);
                decompileAt(target, "slot " + i);
            }
            for (int i = 3; i < args.length; i++) decompileAt(toAddr(Long.parseLong(args[i], 16)), "requested");
            dec.dispose();
        }
    }
}
