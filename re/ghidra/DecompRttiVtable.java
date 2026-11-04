// For each C++ class name: finds its MSVC RTTI type descriptor (".?AV<name>@@"), the complete object locators that
// point to it, the vtables right after a pointer to those locators, and decompiles every slot up to the first
// non-code pointer. For classes without the game's own type registration (Havok's hkp*).
// Headless: -postScript DecompRttiVtable.java <out file> <class name> [<class name> ...]
// @category coop
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import java.io.PrintWriter;
import java.util.*;

public class DecompRttiVtable extends GhidraScript {
    private DecompInterface dec;
    private PrintWriter out;

    private boolean isCode(Address a) {
        MemoryBlock b = getMemoryBlock(a);
        return b != null && b.isExecute();
    }

    private List<Address> findBytes(byte[] pat) throws Exception {
        List<Address> r = new ArrayList<>();
        Address a = currentProgram.getMinAddress();
        while (a != null) {
            a = currentProgram.getMemory().findBytes(a, pat, null, true, monitor);
            if (a == null) break;
            r.add(a);
            a = a.add(1);
        }
        return r;
    }

    private byte[] le32(long v) {
        return new byte[] { (byte) v, (byte) (v >> 8), (byte) (v >> 16), (byte) (v >> 24) };
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        out = new PrintWriter(args[0], "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            String name = ".?AV" + args[i] + "@@";
            out.println("########## class " + args[i]);
            for (Address s : findBytes(name.getBytes("ASCII"))) {
                Address td = s.subtract(8);  // TypeDescriptor: vtable ptr, spare, name
                out.println("# type descriptor " + td);
                for (Address ref : findBytes(le32(td.getOffset()))) {
                    Address col = ref.subtract(12);  // CompleteObjectLocator: sig, offset, cdOffset, pTypeDescriptor
                    if (getInt(col) != 0 || getInt(col.add(4)) != 0) continue;  // primary vtable only
                    for (Address vp : findBytes(le32(col.getOffset()))) {
                        Address vt = vp.add(4);
                        out.println("## vtable " + vt + " (locator " + col + ")");
                        for (int k = 0; k < 40; k++) {
                            Address fa = toAddr(getInt(vt.add(4 * k)) & 0xffffffffL);
                            if (!isCode(fa)) break;
                            Function f = getFunctionAt(fa);
                            if (f == null) f = createFunction(fa, null);
                            out.println("=== slot " + k + " " + fa + (f == null ? " (no function)" : " " + f.getName()));
                            if (f == null) continue;
                            DecompileResults r = dec.decompileFunction(f, 30, monitor);
                            if (r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
                        }
                    }
                }
            }
        }
        out.close();
    }
}
