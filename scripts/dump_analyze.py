"""Quick minidump analyzer for the AI-Worker AV crashes.

Usage:
    py scripts/dump_analyze.py <path-to-dump.dmp>

Reports:
    - Exception code, address, originating thread
    - Module that owns the faulting address (which DLL/exe)
    - Loaded modules with bases & sizes
    - Symbolicated frame if PDB is alongside the EXE/DLL (best-effort)

This is a stand-in for cdb/WinDbg until the user installs the
"Debugging Tools for Windows" component of the Win10 SDK.
"""
from __future__ import annotations

import sys
from pathlib import Path

from minidump.minidumpfile import MinidumpFile


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    dump = Path(sys.argv[1])
    mf = MinidumpFile.parse(str(dump))

    excinfo = getattr(mf, "exception", None)
    if excinfo is not None and excinfo.exception_records:
        rec = excinfo.exception_records[0].ExceptionRecord
        code = rec.ExceptionCode
        # ExceptionCode in this lib is an enum-like wrapper around .value
        code_int = getattr(code, "value", code)
        if not isinstance(code_int, int):
            code_int = int(str(code_int), 0) if str(code_int).startswith("0x") else 0
        code_int &= 0xFFFFFFFF
        addr = rec.ExceptionAddress
        tid = excinfo.exception_records[0].ThreadId
        print("=== Exception ===")
        print(f"  code     : 0x{code_int:08X} ({code!r})")
        print(f"  address  : 0x{addr:016X}")
        print(f"  thread   : {tid}")
        # ExceptionInformation: for AV, [0] = read/write flag, [1] = bad address
        if rec.NumberParameters >= 2:
            kind = rec.ExceptionInformation[0]
            kind_s = {0: "READ", 1: "WRITE", 8: "DEP/EXEC"}.get(kind, f"?({kind})")
            bad = rec.ExceptionInformation[1] & 0xFFFFFFFFFFFFFFFF
            print(f"  AV access: {kind_s} at 0x{bad:016X}")
    else:
        print("No exception record in dump.")
        addr = 0
        tid = -1

    # Find module owning the faulting address
    print()
    print("=== Faulting module ===")
    if addr and mf.modules and mf.modules.modules:
        for m in mf.modules.modules:
            base = m.baseaddress
            size = m.size
            if base <= addr < base + size:
                offset = addr - base
                print(f"  {m.name}")
                print(f"  base    : 0x{base:016X}")
                print(f"  size    : 0x{size:08X}")
                print(f"  offset  : 0x{offset:08X}  <-- look this up against the PDB")
                break
        else:
            print(f"  (no module covers 0x{addr:016X} — kernel/JIT/heap?)")

    print()
    print("=== Loaded modules (top 30) ===")
    if mf.modules and mf.modules.modules:
        rows = sorted(mf.modules.modules, key=lambda m: m.baseaddress)
        for m in rows[:30]:
            print(f"  0x{m.baseaddress:016X}  +0x{m.size:08X}  {m.name}")

    # Faulting thread stack
    print()
    print("=== Faulting thread stack frames (raw) ===")
    if mf.threads and mf.threads.threads:
        target = None
        for t in mf.threads.threads:
            if t.ThreadId == tid:
                target = t
                break
        if target is None:
            print(f"  thread {tid} not in dump.threads — checking ExceptionThread instead")
        else:
            ctx = target.ContextObject
            if ctx is not None:
                rip = getattr(ctx, "Rip", None)
                rsp = getattr(ctx, "Rsp", None)
                rbp = getattr(ctx, "Rbp", None)
                if rip is not None:
                    print(f"  RIP = 0x{rip:016X}")
                if rsp is not None:
                    print(f"  RSP = 0x{rsp:016X}")
                if rbp is not None:
                    print(f"  RBP = 0x{rbp:016X}")
                # The minidump library does not unwind frames itself; print raw
                # stack memory pointers near RSP for manual triage.
                print("  (minidump lib doesn't unwind — use cdb '!analyze -v' for full stack)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
