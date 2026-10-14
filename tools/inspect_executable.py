"""Inspect PS5 executables and game modules without modifying them.

Read-only diagnostics for ``eboot.bin``, ELF files and game modules. The
format verdict is derived from file *content* (magic bytes and ELF header
fields), never from the file name or extension. This mirrors the checks in
``core/relinker/relinker/src/parsing/ElfReader.cpp`` so that a SELF input is
rejected here instead of wasting a Relinker iteration.

Formats:
  ELF        clean ELF (further header validation decides ``ok``)
  SELF       PS5 SELF container (app or kernel magic)
  PKG        PKG container (not an executable image)
  UNKNOWN    none of the known magics
  TRUNCATED  known ELF magic but shorter than the 64-byte ELF header
  TOO_SMALL  shorter than 4 bytes, no magic to classify
  MISSING    path does not exist
  UNREADABLE cannot be read
"""

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ELF_MAGIC = b"\x7fELF"
# Little-endian u32, same values as ElfReader.cpp / SelfInspector.cpp.
SELF_APP_MAGIC = 0x1D3D154F  # bytes 4F 15 3D 1D
SELF_KERNEL_MAGIC = 0xEEF51454  # bytes 54 14 F5 EE
PKG_MAGIC = b"\x7fCNT"
EM_X86_64 = 62
ELF_HEADER_SIZE = 0x40
ELF_PHENTSIZE = 56
ELF_SHENTSIZE = 64
MAX_PHNUM = 64
MAX_SHNUM = 1024

# Module directories honoured by GuestModuleBuilder.cpp (plus --module-dir).
KNOWN_MODULE_DIRS = ("sce_module", "sce_modules", "prx")

CLASS_NAMES = {0: "none", 1: "32-bit", 2: "64-bit"}
DATA_NAMES = {0: "none", 1: "little-endian", 2: "big-endian"}
ETYPE_NAMES = {0: "NONE", 1: "REL", 2: "EXEC", 3: "DYN", 4: "CORE"}
PTYPE_NAMES = {0: "NULL", 1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE",
               6: "PHDR", 7: "TLS", 0x6474E550: "GNU_EHFRAME",
               0x6474E551: "GNU_STACK", 0x6474E552: "GNU_RELRO"}


def head_hex(data, length=16):
    """Lowercase spaced hex of the first bytes, e.g. ``7f 45 4c 46``."""
    return " ".join(f"{byte:02x}" for byte in data[:length])


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inspect_file(path):
    """Inspect a single file. Never modifies it. Returns a plain dict."""
    result = {
        "path": str(path),
        "exists": False,
        "readable": False,
        "size": 0,
        "head_hex": "",
        "format": "MISSING",
        "ok": False,
        "relinkable": False,
        "reason": "file does not exist",
        "elf_class": None,
        "endianness": None,
        "machine": None,
        "version": None,
        "packed_or_encrypted": None,
        "supported_ops": [],
    }
    candidate = Path(path)
    if not candidate.exists():
        return result
    result["exists"] = True
    try:
        data = candidate.read_bytes()
    except OSError as error:
        result["format"] = "UNREADABLE"
        result["reason"] = f"cannot read file: {error}"
        return result
    result["readable"] = True
    result["size"] = len(data)
    result["head_hex"] = head_hex(data)
    if len(data) < 4:
        result["format"] = "TOO_SMALL"
        result["reason"] = "file smaller than 4 bytes: no magic to classify"
        return result
    magic_u32 = struct.unpack_from("<I", data, 0)[0]
    if magic_u32 in (SELF_APP_MAGIC, SELF_KERNEL_MAGIC):
        result["format"] = "SELF"
        result["version"] = "unknown (see self_inspector --report for prologue detail)"
        result["packed_or_encrypted"] = (
            "likely: SELF segment records carry an encrypted flag "
            "(see self_inspector --report); no crypto is implemented here")
        result["supported_ops"] = ["inspect", "extract-via-self_inspector"]
        result["reason"] = (
            "The input is a SELF container, not an ELF. "
            "Relinker needs a clean ELF; convert with the project's "
            "self_inspector plaintext remap (no crypto) into a separate "
            "directory first, or dump a clean ELF. Renaming or copying "
            "the file does not convert it."
        )
        return result
    if data[:4] == PKG_MAGIC:
        result["format"] = "PKG"
        result["supported_ops"] = []
        result["reason"] = (
            f"Invalid ELF magic number: {head_hex(data, 4)}. "
            "PKG container: not an executable image."
        )
        return result
    if data[:4] != ELF_MAGIC:
        if len(data) < ELF_HEADER_SIZE:
            result["format"] = "TOO_SMALL"
            result["reason"] = "file too small for a header and has no known magic"
        else:
            result["format"] = "UNKNOWN"
            result["reason"] = f"Invalid ELF magic number: {head_hex(data, 4)}"
        return result
    if len(data) < ELF_HEADER_SIZE:
        result["format"] = "TRUNCATED"
        result["reason"] = "File too small for ELF header"
        return result
    result["format"] = "ELF"
    elf_class = data[4]
    elf_data = data[5]
    elf_version = data[6]
    result["elf_class"] = CLASS_NAMES.get(elf_class, f"unknown({elf_class})")
    result["endianness"] = DATA_NAMES.get(elf_data, f"unknown({elf_data})")
    machine, = struct.unpack_from("<H", data, 0x12)
    result["machine"] = machine
    problems = []
    if elf_class != 2:
        problems.append(f"not 64-bit class (EI_CLASS={elf_class})")
    if elf_data != 1:
        problems.append(f"not little-endian (EI_DATA={elf_data})")
    if elf_version != 1:
        problems.append(f"bad ELF version (EI_VERSION={elf_version})")
    if machine != EM_X86_64:
        problems.append(f"unexpected machine {machine} (expected {EM_X86_64}/x86-64)")
    ehsize, phentsize, phnum = struct.unpack_from("<HHH", data, 0x34)
    if ehsize != 64:
        problems.append(f"bad ELF header size ehsize={ehsize} (expected 64)")
    if phnum != 0 and phentsize != ELF_PHENTSIZE:
        problems.append(
            f"bad program header entry size phentsize={phentsize} (expected {ELF_PHENTSIZE})"
        )
    if not problems and phnum != 0:
        phoff, = struct.unpack_from("<Q", data, 0x20)
        if phoff + phnum * phentsize > len(data):
            problems.append("program header table out of file bounds")
    if problems:
        result["reason"] = "plain ELF with invalid headers: " + "; ".join(problems)
        result["supported_ops"] = []
        return result
    parse_problems, section_warnings, tables = _parse_elf_tables(data)
    problems.extend(parse_problems)
    result.update(tables)
    result["section_warnings"] = section_warnings
    result["version"] = f"EV_CURRENT(1)"
    result["packed_or_encrypted"] = (
        "not detected: plain ELF carries no encryption markers; "
        "packer heuristics are not implemented and no claim is made")
    if problems:
        result["reason"] = "plain ELF with invalid structure: " + "; ".join(problems)
        result["supported_ops"] = []
        return result
    result["supported_ops"] = ["relink"]
    result["ok"] = True
    result["relinkable"] = True
    result["reason"] = "clean ELF64 little-endian x86-64 executable object"
    return result


def _parse_elf_tables(data):
    """Bounds-checked program/section header tables.

    Returns (fatal_problems, section_warnings, tables). Reads at most the two
    tables; never scans the whole file. Mirrors the layout expectations of
    ElfReader.cpp (64-byte ehdr, 56-byte phdr).

    Section-header issues are warnings only: Relinker's ElfReader defines
    ReadSectionHeaders but no pipeline stage calls it, so a dangling section
    table (typical for SELF-extracted images) must not block conversion.
    """
    problems = []
    section_warnings = []
    size = len(data)
    etype, = struct.unpack_from("<H", data, 0x10)
    entry, = struct.unpack_from("<Q", data, 0x18)
    phoff, = struct.unpack_from("<Q", data, 0x20)
    shoff, = struct.unpack_from("<Q", data, 0x28)
    ehsize, phentsize, phnum = struct.unpack_from("<HHH", data, 0x34)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    tables = {
        "elf_type": ETYPE_NAMES.get(etype, f"unknown({etype})"),
        "entry": entry,
        "program_headers": [],
        "section_header_count": shnum,
        "has_dynamic": False,
        "has_interp": False,
    }
    if phnum > MAX_PHNUM:
        problems.append(f"program header count {phnum} exceeds sanity limit {MAX_PHNUM}")
        return problems, section_warnings, tables
    if phnum != 0:
        if phoff + phnum * phentsize > size:
            problems.append("program header table out of file bounds")
            return problems, section_warnings, tables
        for i in range(phnum):
            at = phoff + i * phentsize
            ptype, flags = struct.unpack_from("<II", data, at)
            offset, vaddr, _, filesz, memsz, align = struct.unpack_from("<QQQQQQ", data, at + 8)
            in_bounds = offset + filesz <= size
            tables["program_headers"].append({
                "index": i,
                "type": PTYPE_NAMES.get(ptype, f"unknown({ptype:#x})"),
                "flags": flags,
                "offset": offset,
                "vaddr": vaddr,
                "filesz": filesz,
                "memsz": memsz,
                "align": align,
                "in_bounds": in_bounds,
            })
            if not in_bounds:
                problems.append(f"program header {i} file range out of bounds")
            if ptype == 2:
                tables["has_dynamic"] = True
            if ptype == 3:
                tables["has_interp"] = True
    if shnum > MAX_SHNUM:
        section_warnings.append(f"section header count {shnum} exceeds sanity limit {MAX_SHNUM}")
        return problems, section_warnings, tables
    if shnum != 0:
        if shentsize != ELF_SHENTSIZE:
            section_warnings.append(
                f"bad section header entry size shentsize={shentsize} (expected 64); "
                "ignored: Relinker never reads section headers")
            return problems, section_warnings, tables
        if shoff + shnum * shentsize > size:
            section_warnings.append(
                "section header table out of file bounds; "
                "ignored: Relinker never reads section headers")
            return problems, section_warnings, tables
        if shstrndx >= shnum:
            section_warnings.append(f"section name string index {shstrndx} out of range ({shnum})")
        for i in range(shnum):
            at = shoff + i * shentsize
            offset, section_size = struct.unpack_from("<QQ", data, at + 24)
            if offset + section_size > size:
                section_warnings.append(f"section header {i} range out of file bounds")
                break
    if phnum != 0 and not tables["has_dynamic"]:
        problems.append("no PT_DYNAMIC program header (Relinker needs the dynamic section)")
    return problems, section_warnings, tables


def inspect_output(path, target):
    """Check a Relinker output file: PE (MZ) for windows, ELF for linux."""
    info = inspect_file(path)
    if target == "windows":
        try:
            data = Path(path).read_bytes()
        except OSError:
            return {"ok": False, "reason": info["reason"], "detail": info}
        ok = data[:2] == b"MZ"
        return {
            "ok": ok,
            "reason": "Windows PE output (MZ signature)" if ok
            else f"expected Windows PE (MZ), got {info['format']} ({info['head_hex']})",
            "detail": info,
        }
    ok = info["format"] == "ELF" and info["ok"]
    return {
        "ok": ok,
        "reason": info["reason"] if ok else f"expected Linux ELF, got: {info['reason']}",
        "detail": info,
    }


def inspect_game_dir(game_dir, extra_module_dirs=()):
    """Inspect game modules beside the input executable (read-only).

    Applies the conventions from docs/user/USAGE.md and
    GuestModuleBuilder.cpp: ``sce_module``/``sce_modules``/``prx`` beside the
    input, ``prx`` may coexist with one of the sce_* dirs, both sce_* dirs at
    once or all three absent is an error.
    """
    root = Path(game_dir)
    report = {
        "root": str(root),
        "exists": root.is_dir(),
        "dirs_found": [],
        "error": "",
        "modules": [],
        "elf_modules": 0,
        "warnings": [],
    }
    if not root.is_dir():
        report["error"] = f"game directory not found: {root}"
        return report
    found = [name for name in KNOWN_MODULE_DIRS if (root / name).is_dir()]
    report["dirs_found"] = found
    if "sce_module" in found and "sce_modules" in found:
        report["error"] = "Both sce_module and sce_modules exist beside the input executable"
        return report
    if not found:
        report["error"] = (
            f"sce_module/sce_modules/prx was not found beside the input executable: {root}. "
            "Use --skip-sce-module only if this game can run without these modules."
        )
        return report
    for name in found:
        for entry in sorted((root / name).iterdir(), key=lambda p: p.name):
            if entry.is_file():
                info = inspect_file(entry)
                info["directory"] = name
                report["modules"].append(info)
                if info["format"] == "ELF" and info["ok"]:
                    report["elf_modules"] += 1
                elif info["format"] == "ELF":
                    report["warnings"].append(f"{entry.name}: invalid ELF headers, Relinker will reject it")
                elif entry.suffix.lower() in (".prx", ".elf", ".bin", ".self", ""):
                    report["warnings"].append(
                        f"{entry.name}: {info['format']} ({info['reason']}); "
                        "non-ELF files are ignored by Relinker"
                    )
    for name in extra_module_dirs:
        relative = Path(name)
        normalized = str(relative).replace("\\", "/")
        if relative.is_absolute() or normalized == ".." or normalized.startswith("../") or "/../" in normalized:
            report["error"] = f"--module-dir must be a directory inside the input directory: {name}"
            return report
        directory = root / relative
        if not directory.is_dir():
            report["error"] = f"Guest module path is not a directory: {directory}"
            return report
        known = {str((root / d)) for d in found}
        if str(directory) in known:
            report["error"] = f"Duplicate guest module directory: {directory}"
            return report
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Read-only inspection of PS5 executables and game modules."
    )
    parser.add_argument("path", help="file to inspect (e.g. eboot.bin)")
    parser.add_argument("--game-dir", default=None, help="game directory holding sce_module/sce_modules/prx")
    parser.add_argument("--module-dir", action="append", default=[], help="extra module dir relative to --game-dir (repeatable)")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    args = parser.parse_args(argv)
    info = inspect_file(args.path)
    modules = None
    if args.game_dir:
        modules = inspect_game_dir(args.game_dir, args.module_dir)
    if args.json:
        print(json.dumps({"file": info, "modules": modules}, indent=2))
    else:
        print(f"path:   {info['path']}")
        print(f"size:   {info['size']} bytes")
        print(f"head:   {info['head_hex']}")
        print(f"format: {info['format']}")
        print(f"status: {'OK (relinkable)' if info['relinkable'] else 'NOT relinkable'}")
        print(f"reason: {info['reason']}")
        if info.get("version"):
            print(f"version: {info['version']}")
        if info.get("elf_type"):
            print(f"type: {info['elf_type']} entry=0x{info['entry']:x} "
                  f"phdrs={len(info['program_headers'])} shnum={info['section_header_count']} "
                  f"dynamic={info['has_dynamic']}")
        for warning in info.get("section_warnings", []):
            print(f"section warning: {warning}")
        if info.get("packed_or_encrypted"):
            print(f"packed/encrypted: {info['packed_or_encrypted']}")
        if info.get("supported_ops"):
            print(f"supported ops: {', '.join(info['supported_ops'])}")
        if modules is not None:
            print(f"modules dirs: {modules['dirs_found'] or 'none'}")
            if modules["error"]:
                print(f"modules error: {modules['error']}")
            print(f"ELF modules: {modules['elf_modules']}")
            for warning in modules["warnings"]:
                print(f"warning: {warning}")
    return 0 if info["relinkable"] else 2


if __name__ == "__main__":
    sys.exit(main())
