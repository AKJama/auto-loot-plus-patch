"""Build and check auto-loot-plus-mod-files.zip, the mod files of a release.

The zip holds every git-tracked file of mod/auto-loot-plus except the player's own (KEPT: config.txt),
under mods/auto-loot-plus/, so unpacking it into the game folder puts each file where the game, EML
and dinput8.dll read it, and keeps the player's settings. REQUIRED says what each file is read for.

The check reads the written zip back and fails when its files are not exactly the shipped ones, a
REQUIRED file is missing, config.txt is in it, or a file differs from the source byte for byte. It
also fails when kModVersion in native/main.cpp and VERSION in mod.lua differ, since the zip ships
that mod.lua next to the DLL. Standard library and git only.

Usage:
  python tools/mod_files_zip.py OUT.zip    build OUT.zip and check it (exit 1 on any problem)
"""
import argparse
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "mod" / "auto-loot-plus"
PREFIX = "mods/auto-loot-plus/"
KEPT = {"config.txt"}   # the player's own files: never in the zip
REQUIRED = {
    "items.txt": "item names: the DLL's \"Ignore items\" suggestions and filter",
    "quest_items.txt": "quest items that stay in chests and corpses (DLL)",
    "templates.txt": "template groups and yields (DLL)",
    "mod.json": "the mod's entry for EML",
    "src/mod.lua": "the Lua part (EML)",
    "src/autosmash_program.lua": "required by mod.lua",
}


def shipped() -> list[str]:
    """Tracked files of SOURCE except KEPT, relative, with forward slashes, sorted."""
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z", "--", SOURCE.relative_to(ROOT).as_posix()],
                         check=True, capture_output=True, text=True).stdout
    rels = (Path(p).relative_to(SOURCE.relative_to(ROOT)).as_posix() for p in out.split("\0") if p)
    return sorted(r for r in rels if r not in KEPT)


def build(out: Path, files: list[str]) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in files:
            # a fixed date, so the same source gives the same zip
            z.writestr(zipfile.ZipInfo(PREFIX + rel, date_time=(1980, 1, 1, 0, 0, 0)), (SOURCE / rel).read_bytes(),
                       compress_type=zipfile.ZIP_DEFLATED)


def versions_differ() -> str | None:
    dll = re.search(r'kModVersion = "([^"]+)"', (ROOT / "native" / "main.cpp").read_text(encoding="utf-8"))
    lua = re.search(r'^local VERSION = "([^"]+)"', (SOURCE / "src" / "mod.lua").read_text(encoding="utf-8"), re.M)
    dll_v, lua_v = dll and dll.group(1), lua and lua.group(1)
    return None if dll_v and dll_v == lua_v else f"kModVersion {dll_v} in main.cpp, VERSION {lua_v} in mod.lua"


def check(zip_path: Path, files: list[str]) -> tuple[list[str], list[zipfile.ZipInfo]]:
    problems = []
    with zipfile.ZipFile(zip_path) as z:
        infos = [i for i in z.infolist() if not i.is_dir()]
        names = {i.filename for i in infos}
        expected = {PREFIX + r for r in files}
        problems += [f"must not ship (the player's own): {n}" for n in sorted(names) if n.removeprefix(PREFIX) in KEPT]
        problems += [f"missing: {r}" for r in sorted(REQUIRED) if PREFIX + r not in names]
        problems += [f"not a shipped file: {n}" for n in sorted(names - expected)]
        problems += [f"not in the zip: {n}" for n in sorted(expected - names)]
        problems += [f"differs from the source: {n}" for n in sorted(names & expected)
                     if z.read(n) != (SOURCE / n.removeprefix(PREFIX)).read_bytes()]
    if differ := versions_differ():
        problems.append(differ)
    return problems, infos


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zip", type=Path)
    args = ap.parse_args()
    files = shipped()
    build(args.zip, files)
    problems, infos = check(args.zip, files)
    for p in problems:
        print(f"FAIL {p}")
    if problems:
        return 1
    for i in infos:
        print(f"{i.file_size:>9}  {i.filename}")
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
