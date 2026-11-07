"""Unripe growth stages of planted crops: check that Auto Loot Plus never loots them on its own.

A planted seedling grows through the templates named in its keen::ecs::Upgrade levels; only the
last level is the ripe plant. Many middle levels are wild-plant templates (the hazelnut's second
level is the twig bush Scatter_Loot_DeepWoods_Hazelnut_a_noharvest_InstantLoot), so ALP 1.1.8
harvested young crops into twigs and destroyed them. 1.1.8-fix3 excludes every middle level:
mod.lua at load time (from the game data) and templates.txt for the DLL (this script).

Input: tools/growth-stages.tsv, written by the EML export in tools/growth-export
(see MAINTAINING.md, "Growth stages"). Standard library only.

Usage:
  python tools/growth_stages.py            check: no unripe stage in templates.txt (exit 1 if any)
  python tools/growth_stages.py --list     every unripe stage, by crop and level
  python tools/growth_stages.py --fix      remove the unripe stages from templates.txt
  python tools/growth_stages.py --report BEFORE.txt [AFTER.txt]
      what mod.lua does with each unripe stage, from auto-loot-plus-report.txt of an export run
      (emm run --export with the mod installed): before and after a change
"""
import argparse
import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STAGES = ROOT / "tools" / "growth-stages.tsv"
TEMPLATES = ROOT / "mod" / "auto-loot-plus" / "templates.txt"


def unripe_stages(path: Path = STAGES) -> dict[str, tuple[str, str, str]]:
    """guid -> (stage name, crop root name, level) for every growth level but the last.

    Only chains whose root grows (keen::ecs::Growth): the base altar also has Upgrade levels.
    The same as mod.lua's unripeStages."""
    out = {}
    with path.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            if row["root_has_growth"] == "1" and row["last"] == "0":
                out[row["stage_guid"]] = (row["stage_name"], row["root_name"], row["level"])
    return out


def template_lines(path: Path = TEMPLATES) -> list[str]:
    return path.read_text(encoding="utf-8").splitlines(keepends=True)


def listed(lines: list[str], unripe: dict) -> list[tuple[str, str]]:
    """(group, name) of each unripe stage templates.txt gives a group (the DLL would loot it)."""
    hits = []
    for line in lines:
        if line.startswith("#"):
            continue
        cols = line.rstrip("\r\n").split("\t")
        if cols[0] in unripe:
            hits.append((cols[1], cols[3] if len(cols) > 3 else ""))
    return sorted(hits)


def report_categories(path: Path, names: set[str]) -> dict[str, str]:
    """name -> mod.lua's category for that template (report lines "category<TAB>name")."""
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        cols = line.split("\t")
        if len(cols) > 1 and cols[1] in names:
            out[cols[1]] = cols[0]
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--report", nargs="+", type=Path, metavar="REPORT")
    args = ap.parse_args()
    unripe = unripe_stages()
    print(f"{len(unripe)} unripe growth stages in {STAGES.name}")

    if args.list:
        for guid, (name, root, level) in sorted(unripe.items(), key=lambda kv: (kv[1][1], int(kv[1][2]))):
            print(f"  {root}\tL{level}\t{name}\t{guid}")

    if args.report:
        names = {name for name, _, _ in unripe.values()}
        cats = [report_categories(p, names) for p in args.report]
        print("mod.lua per unripe stage: " + " -> ".join(p.name for p in args.report) + " (- = not handled)")
        for name in sorted(names):
            row = [c.get(name, "-") for c in cats]
            if any(r != "-" for r in row):
                print(f"  {' -> '.join(row):40s} {name}")

    lines = template_lines()
    hits = listed(lines, unripe)
    if args.fix and hits:
        kept = [l for l in lines if l.startswith("#") or l.split("\t", 1)[0] not in unripe]
        TEMPLATES.write_bytes("".join(kept).encode("utf-8"))
        print(f"templates.txt: removed {len(lines) - len(kept)} unripe stages:")
        for group, name in hits:
            print(f"  {group}\t{name}")
        hits = listed(template_lines(), unripe)
    if hits:
        print(f"FAIL: templates.txt gives {len(hits)} unripe stages a group (the DLL loots them):")
        for group, name in hits:
            print(f"  {group}\t{name}")
        return 1
    print("ok: no unripe stage in templates.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
