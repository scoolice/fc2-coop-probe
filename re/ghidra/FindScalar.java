// Lists every instruction with one of the given constants as an operand, with the function it is in.
// Headless: -postScript FindScalar.java <out file> <value hex> [<value hex> ...]   (e.g. 121ade93)
// @category coop

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

public class FindScalar extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        long[] want = new long[args.length - 1];
        for (int i = 1; i < args.length; i++) want[i - 1] = Long.parseUnsignedLong(args[i], 16);
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Instruction ins = it.next();
                for (int op = 0; op < ins.getNumOperands(); op++) {
                    for (Object o : ins.getOpObjects(op)) {
                        if (!(o instanceof Scalar)) continue;
                        long v = ((Scalar) o).getUnsignedValue() & 0xffffffffL;
                        for (long w : want) {
                            if (v != w) continue;
                            Function f = getFunctionContaining(ins.getAddress());
                            out.println(Long.toHexString(w) + "  " + ins.getAddress() + "  " + ins + "  in " + (f == null ? "(no function)" : f.getName() + " @ " + f.getEntryPoint()));
                        }
                    }
                }
            }
        }
    }
}
