// Lists every 4-byte-aligned place in initialized memory that holds one of the given values (e.g. a function
// address inside a vtable), with the label of the data or function there.
// Headless: -postScript FindPtr.java <out file> <value hex> [<value hex> ...]   (e.g. 101550f0)
// @category coop

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.mem.MemoryBlock;

public class FindPtr extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                int v = (int) Long.parseLong(args[i], 16);
                byte[] pat = { (byte) v, (byte) (v >> 8), (byte) (v >> 16), (byte) (v >> 24) };
                for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                    if (!b.isInitialized()) continue;
                    Address a = b.getStart();
                    while (a != null && a.compareTo(b.getEnd()) <= 0 && !monitor.isCancelled()) {
                        Address hit = currentProgram.getMemory().findBytes(a, b.getEnd(), pat, null, true, monitor);
                        if (hit == null) break;
                        if ((hit.getOffset() & 3) == 0) {
                            out.println(args[i] + "  at " + hit + "  (" + b.getName() + ")");
                        }
                        a = hit.add(1);
                    }
                }
            }
        }
    }
}
