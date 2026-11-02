// Prints the disassembly from <before> bytes ahead of each given address to <after> bytes past it, disassembling
// on the fly where Ghidra has no instructions (for code that belongs to no function).
// Headless: -postScript DisasmRange.java <out file> <before> <after> <address> [<address> ...]   (hex)
// @category coop
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import java.io.PrintWriter;

public class DisasmRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        long before = Long.parseLong(args[1], 16), after = Long.parseLong(args[2], 16);
        try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 3; i < args.length; i++) {
                Address at = toAddr(args[i]);
                w.println("=== " + at + " (-" + args[1] + " / +" + args[2] + ")");
                Address a = at.subtract(before), end = at.add(after);
                while (a.compareTo(end) < 0) {
                    Instruction ins = getInstructionAt(a);
                    if (ins == null) {
                        disassemble(a);
                        ins = getInstructionAt(a);
                    }
                    if (ins == null) {
                        w.println(a + "  ?? " + String.format("%02x", getByte(a) & 0xff));
                        a = a.add(1);
                        continue;
                    }
                    StringBuilder refs = new StringBuilder();
                    for (var r : ins.getReferencesFrom()) {
                        var d = getDataAt(r.getToAddress());
                        if (d != null && d.getValue() instanceof String s) refs.append("  \"").append(s).append('"');
                        var f = getFunctionAt(r.getToAddress());
                        if (f != null) refs.append("  <").append(f.getName()).append('>');
                    }
                    w.println((a.equals(at) ? "> " : "  ") + a + "  " + ins + refs);
                    a = a.add(ins.getLength());
                }
                w.println();
            }
        }
    }
}
