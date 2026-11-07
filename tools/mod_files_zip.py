"""Build and check auto-loot-plus-mod-files.zip, the mod files of a release.

The zip holds every file of mod/auto-loot-plus except config.txt, under mods/auto-loot-plus/, so
unpacking it into the game folder puts each file where the game, EML and dinput8.dll read it:
  - items.txt        item names (DLL: the "Ignore items" suggestions and filter)
  - quest_items.txt  quest items that stay in chests and corpses (DLL)
  - templates.txt    template groups and yields (DLL)
  - mod.json         the mod's entry for EML
  - src/mod.lua, src/autosmash_program.lua   the Lua part (EML)
config.txt holds the player's settings and is never shipped, so unpacking keeps them.

The check reads the written zip back and fails when a required file is missing, config.txt is in
it, or a file differs from the source byte for byte. Standard library only.

Usage:
  python tools/mod_files_zip.py OUT.zip       build OUT.zip and check it (exit 1 on any problem)
  python tools/mod_files_zip.py --check ZIP   check an existing zip
"""
import argparse
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "mod" / "auto-loot-plus"
PREFIX = "mods/auto-loot-plus/"
KEPT = {"config.txt"}   # the player's own files: never in the zip
REQUIRED = {"items.txt", "quest_items.txt", "templates.txt", "mod.json", "src/mod.lua", "src/autosmash_program.lua"}


def shipped() -> list[str]:
    """Paths relative to SOURCE, with forward slashes, sorted."""
    return sorted(p.relative_to(SOURCE).as_posix() for p in SOURCE.rglob("*")
                  if p.is_file() and p.relative_to(SOURCE).as_posix() not in KEPT)


def build(out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in shipped():
            # a fixed date, so the same source gives the same zip
            info = zipfile.ZipInfo(PREFIX + rel, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, (SOURCE / rel).read_bytes())


def check(zip_path: Path) -> list[str]:
    problems = []
    with zipfile.ZipFile(zip_path) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        outside = [n for n in names if not n.startswith(PREFIX)]
        problems += [f"outside {PREFIX}: {n}" for n in outside]
        rels = {n[len(PREFIX):] for n in names if n.startswith(PREFIX)}
        problems += [f"must not ship (the player's own): {k}" for k in sorted(KEPT & rels)]
        problems += [f"missing: {r}" for r in sorted(REQUIRED - rels)]
        for rel in sorted(rels - KEPT):
            src = SOURCE / rel
            if not src.is_file():
                problems.append(f"not in {SOURCE.relative_to(ROOT).as_posix()}: {rel}")
            elif z.read(PREFIX + rel) != src.read_bytes():
                problems.append(f"differs from the source: {rel}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zip", type=Path)
    ap.add_argument("--check", action="store_true", help="only check an existing zip")
    args = ap.parse_args()
    if not args.check:
        build(args.zip)
    problems = check(args.zip)
    for p in problems:
        print(f"FAIL {p}")
    if problems:
        return 1
    with zipfile.ZipFile(args.zip) as z:
        for i in z.infolist():
            print(f"{i.file_size:>9}  {i.filename}")
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
