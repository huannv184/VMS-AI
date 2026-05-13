"""Resolve a faulting address to a function name + line via dbghelp.dll.

Standalone replacement for `cdb -c '!analyze -v'` when Debugging Tools for
Windows are not installed. Uses the OS-provided dbghelp.dll + the PDB that
sits next to the binary.

Usage:
    py scripts/symbolize.py <exe_or_dll> <pdb_dir> <faulting_va>

Example:
    py scripts/symbolize.py \
        cpp-backend/build/Release/ai_worker_v2.exe \
        cpp-backend/build/Release \
        0x7FF760436BC4
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import sys

dbghelp = ctypes.WinDLL("dbghelp.dll")
kernel32 = ctypes.WinDLL("kernel32.dll")

# SYMOPT_ values
SYMOPT_LOAD_LINES   = 0x00000010
SYMOPT_UNDNAME      = 0x00000002
SYMOPT_DEFERRED_LOADS = 0x00000004

class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct",  wt.ULONG),
        ("TypeIndex",     wt.ULONG),
        ("Reserved",      ctypes.c_ulonglong * 2),
        ("Index",         wt.ULONG),
        ("Size",          wt.ULONG),
        ("ModBase",       ctypes.c_ulonglong),
        ("Flags",         wt.ULONG),
        ("Value",         ctypes.c_ulonglong),
        ("Address",       ctypes.c_ulonglong),
        ("Register",      wt.ULONG),
        ("Scope",         wt.ULONG),
        ("Tag",           wt.ULONG),
        ("NameLen",       wt.ULONG),
        ("MaxNameLen",    wt.ULONG),
        ("Name",          ctypes.c_char * 1024),
    ]

class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.ULONG),
        ("Key",          ctypes.c_void_p),
        ("LineNumber",   wt.DWORD),
        ("FileName",     ctypes.c_char_p),
        ("Address",      ctypes.c_ulonglong),
    ]

dbghelp.SymInitialize.argtypes        = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
dbghelp.SymInitialize.restype         = wt.BOOL
dbghelp.SymSetOptions.argtypes        = [wt.DWORD]
dbghelp.SymSetOptions.restype         = wt.DWORD
dbghelp.SymLoadModuleExW.argtypes     = [wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR,
                                          ctypes.c_ulonglong, wt.DWORD, ctypes.c_void_p, wt.DWORD]
dbghelp.SymLoadModuleExW.restype      = ctypes.c_ulonglong
dbghelp.SymFromAddr.argtypes          = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong),
                                          ctypes.POINTER(SYMBOL_INFO)]
dbghelp.SymFromAddr.restype           = wt.BOOL
dbghelp.SymGetLineFromAddr64.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(wt.DWORD),
                                          ctypes.POINTER(IMAGEHLP_LINE64)]
dbghelp.SymGetLineFromAddr64.restype  = wt.BOOL
dbghelp.SymCleanup.argtypes           = [wt.HANDLE]
dbghelp.SymCleanup.restype            = wt.BOOL


class IMAGEHLP_MODULE64(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct",  wt.ULONG),
        ("BaseOfImage",   ctypes.c_ulonglong),
        ("ImageSize",     wt.DWORD),
        ("TimeDateStamp", wt.DWORD),
        ("CheckSum",      wt.DWORD),
        ("NumSyms",       wt.DWORD),
        ("SymType",       wt.DWORD),
        ("ModuleName",    ctypes.c_char * 32),
        ("ImageName",     ctypes.c_char * 256),
        ("LoadedImageName", ctypes.c_char * 256),
        ("LoadedPdbName", ctypes.c_char * 256),
        ("CVSig",         wt.DWORD),
        ("CVData",        ctypes.c_char * (780 * 3)),
        ("PdbSig",        wt.DWORD),
        ("PdbSig70",      ctypes.c_char * 16),
        ("PdbAge",        wt.DWORD),
        ("PdbUnmatched",  wt.BOOL),
        ("DbgUnmatched",  wt.BOOL),
        ("LineNumbers",   wt.BOOL),
        ("GlobalSymbols", wt.BOOL),
        ("TypeInfo",      wt.BOOL),
        ("SourceIndexed", wt.BOOL),
        ("Publics",       wt.BOOL),
    ]


dbghelp.SymGetModuleInfo64.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(IMAGEHLP_MODULE64)]
dbghelp.SymGetModuleInfo64.restype  = wt.BOOL


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2

    exe = os.path.abspath(sys.argv[1])
    pdb_dir = os.path.abspath(sys.argv[2])
    addr = int(sys.argv[3], 0)

    h = wt.HANDLE(1)  # pseudo handle, dbghelp accepts any unique non-zero
    # No DEFERRED_LOADS — for one-shot lookups we want the PDB resolved upfront
    # so we get a clear error if it's missing/mismatched.
    dbghelp.SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME)

    if not dbghelp.SymInitialize(h, pdb_dir.encode("mbcs"), False):
        err = ctypes.get_last_error()
        print(f"SymInitialize failed: {err}")
        return 1

    # Load the module at the address it was actually loaded at when the dump
    # was captured. This is the key: the .exe in the dump was loaded at
    # 0x7FF760400000, so we must load it at the same base for the addr lookup
    # to work. Pass DllSize=0 → dbghelp reads it from the PE.
    base_va_in_dump = 0x00007FF760400000  # from cam2 dump; both dumps match.
    base = dbghelp.SymLoadModuleExW(h, None, exe, None, base_va_in_dump, 0, None, 0)
    if base == 0:
        err = ctypes.get_last_error()
        print(f"SymLoadModuleExW failed: {err}")
        return 1
    print(f"Module loaded at 0x{base:016X}")

    # Verify PDB actually matched (not just module mapped)
    info = IMAGEHLP_MODULE64()
    info.SizeOfStruct = ctypes.sizeof(IMAGEHLP_MODULE64)
    if dbghelp.SymGetModuleInfo64(h, base, ctypes.byref(info)):
        sym_types = {0:"None",1:"COFF",2:"CodeView",3:"Pdb",4:"Export",5:"Deferred",6:"Sym",7:"Dia",8:"Virtual"}
        print(f"  SymType    : {sym_types.get(info.SymType, info.SymType)}")
        print(f"  PDB loaded : {info.LoadedPdbName.decode('mbcs', errors='replace') or '(none)'}")
        print(f"  unmatched  : pdb={bool(info.PdbUnmatched)} dbg={bool(info.DbgUnmatched)}")
        print(f"  lines      : {bool(info.LineNumbers)}")
        print(f"  numsyms    : {info.NumSyms}")
    else:
        err = ctypes.get_last_error()
        print(f"  SymGetModuleInfo64 failed: {err}")

    # Resolve symbol — SizeOfStruct must equal the SYMBOL_INFO header size
    # WITHOUT the trailing Name buffer. Windows defines Name as CHAR[1] in the
    # struct, but expects the caller to over-allocate and pass MaxNameLen so
    # SymFromAddr writes the function name into the trailing bytes.
    sym = SYMBOL_INFO()
    name_offset = SYMBOL_INFO.Name.offset  # offset of the Name array in our layout
    sym.SizeOfStruct = name_offset + 1     # header + 1 byte (CHAR Name[1])
    sym.MaxNameLen = 1024
    displ = ctypes.c_ulonglong(0)
    if dbghelp.SymFromAddr(h, addr, ctypes.byref(displ), ctypes.byref(sym)):
        name = sym.Name.decode("mbcs", errors="replace")
        print(f"\n=== Symbol at 0x{addr:016X} ===")
        print(f"  function : {name}")
        print(f"  symbol VA: 0x{sym.Address:016X}")
        print(f"  +disp    : 0x{displ.value:X}")
    else:
        err = ctypes.get_last_error()
        print(f"SymFromAddr failed: err={err}")

    line = IMAGEHLP_LINE64()
    line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
    col = wt.DWORD(0)
    if dbghelp.SymGetLineFromAddr64(h, addr, ctypes.byref(col), ctypes.byref(line)):
        fname = line.FileName.decode("mbcs", errors="replace") if line.FileName else "?"
        print(f"  source   : {fname}:{line.LineNumber}")
    else:
        err = ctypes.get_last_error()
        print(f"  (no line info: err={err})")

    dbghelp.SymCleanup(h)
    return 0


if __name__ == "__main__":
    sys.exit(main())
