#!/usr/bin/env python3
"""Verify that every signature in plugins/game_fixes/assets/gamedata.jsonc matches
exactly once in the target game libraries, and report the file offset -> VA mapping.

Usage: python3 -I verify_signatures.py <gamedata.jsonc> <libserver.so|libengine2.so> ...

The gamedata.jsonc is the s2sdk format:
  csgo -> Signatures -> <name> -> { "library": "server|engine2", "linuxsteamrt64": "48 89 ..." }
`?` in a pattern is a wildcard byte.

For each match the script prints the file offset, the VA (file offset mapped through the
ELF PT_LOAD segment that contains it) and the .text section start VA, so results from this
copy of the binary can be compared with the live server's /srv/cs2-go copy.
"""

import json
import re
import struct
import sys


def strip_jsonc(text: str) -> str:
    # Remove // comments (naive: fine for the s2sdk gamedata files, which have no // inside strings)
    out = []
    in_string = False
    escape = False
    i = 0
    while i < len(text):
        c = text[i]
        if in_string:
            out.append(c)
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < len(text) and text[i + 1] == "/":
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def parse_pattern(pattern: str):
    """Return list of (byte or None) for each position."""
    parts = pattern.split()
    out = []
    for p in parts:
        if p in ("?", "??", "**"):
            out.append(None)
        else:
            out.append(int(p, 16))
    return out


def find_all(data: bytes, pattern):
    # index positions of the first fixed byte to cut scan cost
    first = None
    for i, b in enumerate(pattern):
        if b is not None:
            first = i
            break
    if first is None:
        raise ValueError("pattern has no fixed bytes")
    hits = []
    start = 0
    needle = bytes([pattern[first]])
    while True:
        idx = data.find(needle, start)
        if idx == -1:
            break
        base = idx - first
        if base >= 0:
            ok = True
            for j, b in enumerate(pattern):
                if b is not None and data[base + j] != b:
                    ok = False
                    break
            if ok:
                hits.append(base)
        start = idx + 1
    return hits


def load_segments(data: bytes):
    """Parse ELF64 program headers: list of (offset, vaddr, filesz)."""
    if data[:4] != b"\x7fELF":
        return []
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for n in range(e_phnum):
        off = e_phoff + n * e_phentsize
        p_type = struct.unpack_from("<I", data, off)[0]
        if p_type != 1:  # PT_LOAD
            continue
        p_offset = struct.unpack_from("<Q", data, off + 0x08)[0]
        p_vaddr = struct.unpack_from("<Q", data, off + 0x10)[0]
        p_filesz = struct.unpack_from("<Q", data, off + 0x20)[0]
        segs.append((p_offset, p_vaddr, p_filesz))
    return segs


def text_section_va(data: bytes):
    """Parse the section headers for .text; return its VA or None."""
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]
    if e_shoff == 0 or e_shnum == 0:
        return None
    sh_str_off = e_shoff + e_shstrndx * e_shentsize
    strtab_off = struct.unpack_from("<Q", data, sh_str_off + 0x18)[0]
    for n in range(e_shnum):
        sh = e_shoff + n * e_shentsize
        name_off = struct.unpack_from("<I", data, sh)[0]
        end = data.find(b"\0", strtab_off + name_off)
        name = data[strtab_off + name_off : end].decode("ascii", "replace")
        if name == ".text":
            return struct.unpack_from("<Q", data, sh + 0x10)[0]
    return None


def offset_to_va(segs, offset):
    for p_offset, p_vaddr, p_filesz in segs:
        if p_offset <= offset < p_offset + p_filesz:
            return p_vaddr + (offset - p_offset)
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    with open(sys.argv[1], "r", encoding="utf-8") as fh:
        cfg = json.loads(strip_jsonc(fh.read()))
    sigs = cfg.get("csgo", {}).get("Signatures", {})

    libs = {}
    for path in sys.argv[2:]:
        with open(path, "rb") as fh:
            data = fh.read()
        segs = load_segments(data)
        libs[path] = (data, segs, text_section_va(data))

    rc = 0
    for name, entry in sigs.items():
        library = entry.get("library", "server")
        pattern = entry.get("linuxsteamrt64")
        if not pattern:
            print(f"{name}: no linuxsteamrt64 pattern")
            rc = 1
            continue
        want = "server" if library == "server" else "engine"
        candidates = [p for p in libs if want in p.lower()]
        if not candidates:
            print(f"{name}: no library file given for library '{library}'")
            rc = 1
            continue
        compiled = parse_pattern(pattern)
        for path in candidates:
            data, segs, text_va = libs[path]
            hits = find_all(data, compiled)
            status = "UNIQUE" if len(hits) == 1 else ("NOT FOUND" if not hits else f"{len(hits)} MATCHES")
            if len(hits) != 1:
                rc = 1
            for h in hits[:4]:
                va = offset_to_va(segs, h)
                tva = None if text_va is None or va is None else va - text_va
                print(
                    f"{name}: {status} file=0x{h:X} va={('0x%X' % va) if va is not None else '?'}"
                    f" text+{('0x%X' % tva) if tva is not None else '?'}"
                    f"  [{path.split('/')[-1]}]"
                )
    return rc


if __name__ == "__main__":
    sys.exit(main())
