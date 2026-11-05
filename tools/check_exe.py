"""Check Auto Loot Plus's patterns against an enshrouded.exe, offline, without running the game.

Replays the lookups of Patch() and PatchQuestFilter() in native/main.cpp on the exe file:
every pattern must be found exactly once (in the magnet window where ALP looks), the six constants
must have the values ALP expects, and the helper calls, the near site, the loot-all block, the gate
tail, the AcceptOffered offset and the areaLoot site must look as ALP assumes.
The patterns and offsets are read from main.cpp, so the check never drifts from the DLL.

The exe is only read. Standard library only.

Usage: python tools/check_exe.py [path\\to\\enshrouded.exe]
Exit code 0 when every check passes, 1 otherwise.
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN_CPP = ROOT / "native" / "main.cpp"
DEFAULT_EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Enshrouded\enshrouded.exe")
MAGNET_SIZE = 0x1000


def read_source(src: str) -> tuple[dict[str, str], dict[str, int]]:
    """(kSig* pattern strings, size_t k* constants) from main.cpp."""
    sigs = {}
    for m in re.finditer(r"static const char\* (kSig\w+)\s*=\s*((?:\s*\"[^\"]*\")+)\s*;", src):
        sigs[m.group(1)] = " ".join(re.findall(r"\"([^\"]*)\"", m.group(2)))
    consts = {m.group(1): int(m.group(2), 0)
              for m in re.finditer(r"static const size_t (k\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;", src)}
    return sigs, consts


def regex(pattern: str) -> re.Pattern:
    parts = []
    for tok in pattern.split():
        parts.append(b"." if tok == "??" else re.escape(bytes([int(tok, 16)])))
    return re.compile(b"".join(parts), re.DOTALL)


class Image:
    """The exe's sections mapped at their RVAs, like the loader does."""

    def __init__(self, data: bytes):
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        self.stamp = struct.unpack_from("<I", data, pe + 8)[0]
        opt = struct.unpack_from("<H", data, pe + 20)[0]
        size = struct.unpack_from("<I", data, pe + 24 + 56)[0]   # SizeOfImage
        self.mem = bytearray(size)
        self.text = (0, 0)
        for k in range(nsec):
            s = pe + 24 + opt + 40 * k
            name = data[s:s + 8].rstrip(b"\0")
            vsize, rva, raw_size, raw = struct.unpack_from("<IIII", data, s + 8)
            n = min(vsize, raw_size)
            self.mem[rva:rva + n] = data[raw:raw + n]
            if name == b".text":
                self.text = (rva, vsize)

    def find(self, pattern: str, start: int | None = None, size: int | None = None) -> list[int]:
        if start is None:
            start, size = self.text
        rx = regex(pattern)
        hits, pos, end = [], start, start + size
        while True:
            m = rx.search(self.mem, pos, end)
            if not m:
                return hits
            hits.append(m.start())
            pos = m.start() + 1

    def i32(self, at: int) -> int:
        return struct.unpack_from("<i", self.mem, at)[0]

    def u32(self, at: int) -> int:
        return struct.unpack_from("<I", self.mem, at)[0]

    def rel(self, at: int, end: int) -> int:
        """Target of a rel32 stored at `at` in an instruction ending at `end`."""
        return end + self.i32(at)

    def float_at_disp(self, disp: int) -> float:
        return struct.unpack_from("<f", self.mem, disp + 4 + self.i32(disp))[0]


class Report:
    def __init__(self):
        self.failed = 0

    def check(self, ok: bool, name: str, detail: str = "") -> bool:
        print(f"  {'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
        if not ok:
            self.failed += 1
        return ok


def check(img: Image, sigs: dict[str, str], c: dict[str, int]) -> int:
    r = Report()

    def unique(name: str, sig: str, start=None, size=None) -> int | None:
        hits = img.find(sigs[sig], start, size)
        where = " in the magnet" if start is not None else ""
        r.check(len(hits) == 1, f"pattern '{name}'", f"{len(hits)} match(es){where}"
                + (f" at RVA {hits[0]:#x}" if len(hits) == 1 else ""))
        return hits[0] if len(hits) == 1 else None

    print("Patterns (Patch(): all must match exactly once, or ALP changes nothing)")
    box = unique("box", "kSigBox")
    inside = (box, MAGNET_SIZE) if box is not None else (None, None)
    radii = unique("radii", "kSigRadii", *inside) if box is not None else None
    query = unique("query", "kSigQuery", *inside) if box is not None else None
    no_pickup = unique("noPickup", "kSigNoPickup", *inside) if box is not None else None
    gate = unique("gate", "kSigGate")
    pull = unique("pull", "kSigPull")
    sight = unique("sight", "kSigSight", *inside) if box is not None else None
    if None in (box, radii, query, no_pickup, gate, pull, sight):
        r.check(False, "core sites", "a pattern is missing; ALP logs 'Nichts geaendert' and patches nothing")
        return r.failed

    print("Constants (the value check in Patch())")
    expect = [("box", box + c["kSigBoxDisp"], 7.0), ("near", radii + c["kSigRadiiNear"], 0.5),
              ("radiusSkill", radii + c["kSigRadiiB"], 3.5), ("radius", radii + c["kSigRadiiA"], 1.75),
              ("pullA", pull + c["kSigPullA"], 1.75), ("pullB", pull + c["kSigPullB"], 3.5)]
    for name, disp, value in expect:
        v = img.float_at_disp(disp)
        r.check(v == value, f"constant '{name}'", f"{v:g} (expected {value:g})")

    print("Sites the stubs rewrite")
    near = no_pickup + c["kSigNoPickupNear"]
    r.check(img.mem[near:near + 6] == bytes.fromhex("440F2FE70F86"), "near site", "comiss xmm12,xmm7; jbe rel32")
    je = no_pickup + c["kSigNoPickupJe"]
    r.check(img.mem[je:je + 2] == b"\x0f\x84", "no-PickupItem je")
    check_call = no_pickup + c["kSigNoPickupCheck"]
    r.check(img.mem[check_call] == 0xE8, "pickup check call")
    get_call, guest_call = no_pickup - 5, no_pickup + 0x7A
    r.check(img.mem[get_call] == 0xE8 and img.mem[guest_call] == 0xE8, "GetComponent / guest calls",
            "call at noPickup-5 and noPickup+0x7A")
    loot_all = no_pickup + c["kSigNoPickupLootAll"]
    entry = img.mem[loot_all:loot_all + 7]
    entry_ok = entry[:3] == b"\x48\x8b\x83" and entry[5:7] == b"\0\0"
    r.check(entry_ok, "loot-all block", f"mov rax,[rbx+{img.u32(loot_all + 3):#x}]" if entry_ok else entry.hex(" "))
    jne = sight + c["kSigSightJne"]
    r.check(img.mem[jne:jne + 2] == b"\x0f\x85", "sight jne")
    call = query + c["kSigQueryCall"]
    r.check(img.mem[call] == 0xE8, "query call")
    r.check(img.mem[gate + 0x1A:gate + 0x20] == bytes.fromhex("B208498BC8E9"), "gate tail", "B2 08 49 8B C8 E9")

    print("Looked up at run time (ALP degrades with a log line if these fail)")
    headers = img.find(sigs["kSigHeader"])
    r.check(len(headers) >= 1, "pattern 'header'", f"{len(headers)} match(es), first used")
    allocs = img.find(sigs["kSigAllocAccept"])
    r.check(len(allocs) == 1, "pattern 'allocAccept'", f"{len(allocs)} match(es)")
    if headers and len(allocs) == 1:
        offset = 0
        for p in range(headers[0], headers[0] + 0x80):
            m = img.mem
            if (m[p] == 0x49 and m[p + 1] == 0x8B and m[p + 2] == 0x8E and m[p + 5] == 0 and m[p + 6] == 0
                    and m[p + 7] == 0xE8 and img.rel(p + 8, p + 12) == allocs[0]):
                offset = img.u32(p + 3)
                break
        r.check(offset != 0, "AcceptOffered output", f"context +{offset:#x}" if offset else "not found")
    areas = img.find(sigs["kSigAreaLoot"])
    area_ok = len(areas) == 1 and call < areas[0] < call + 0x200
    r.check(area_ok, "pattern 'areaLoot'", f"{len(areas)} match(es)"
            + (f", context +{img.u32(areas[0] + 3):#x}" if area_ok else ", must be one, within 0x200 after the query call"))
    slots = img.find(sigs["kSigSlotLoop"])
    slot_ok = len(slots) == 1 and img.mem[slots[0] + c["kSlotLoopSite"] + 3:slots[0] + c["kSlotLoopSite"] + 5] == b"\x0f\x84"
    r.check(slot_ok, "pattern 'slotLoop'", f"{len(slots)} match(es)")
    return r.failed


NEEDED_SIGS = ("kSigBox", "kSigRadii", "kSigQuery", "kSigNoPickup", "kSigGate", "kSigPull", "kSigSight",
               "kSigHeader", "kSigAllocAccept", "kSigAreaLoot", "kSigSlotLoop")
NEEDED_CONSTS = ("kSigBoxDisp", "kSigRadiiNear", "kSigRadiiB", "kSigRadiiA", "kSigQueryCall", "kSigNoPickupJe",
                 "kSigNoPickupCheck", "kSigNoPickupNear", "kSigNoPickupLootAll", "kSigSightJne", "kSigPullA",
                 "kSigPullB", "kSlotLoopSite")


def main(argv: list[str]) -> int:
    sigs, consts = read_source(MAIN_CPP.read_text(encoding="utf-8"))
    missing = [n for n in NEEDED_SIGS if n not in sigs] + [n for n in NEEDED_CONSTS if n not in consts]
    if missing:
        print(f"main.cpp no longer defines {', '.join(missing)}: update tools/check_exe.py with Patch()")
        return 1
    if argv == ["--parse-only"]:   # CI: the script still reads main.cpp (there is no exe there)
        print(f"main.cpp: {len(NEEDED_SIGS)} patterns and {len(NEEDED_CONSTS)} offsets read")
        return 0
    exe_path = Path(argv[0]) if argv else DEFAULT_EXE
    img = Image(exe_path.read_bytes())
    version = re.search(r"kModVersion\[?\]?\s*=\s*\"([^\"]+)\"", MAIN_CPP.read_text(encoding="utf-8"))
    print(f"{exe_path}: build stamp {img.stamp:08x}; ALP native {version.group(1) if version else '?'}")
    failed = check(img, sigs, consts)
    print("All checks pass." if not failed else f"{failed} check(s) failed.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
