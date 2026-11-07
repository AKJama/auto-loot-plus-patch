"""Growth stages of planted crops: check that Auto Loot Plus harvests them only when ripe.

A planted seedling grows through the templates named in its keen::ecs::Upgrade levels; only the
last level is the ripe plant. Many middle levels are wild-plant templates (the hazelnut's second
level is the twig bush Scatter_Loot_DeepWoods_Hazelnut_a_noharvest_InstantLoot), so ALP 1.1.8
harvested young crops into twigs and destroyed them. And some ripe levels are templates of their
own that 1.1.8 never listed (the planted hazelnut ripens into ..._Playerplanted_Refill), so ALP
left them alone. 1.1.8-fix3 handles both: mod.lua at load time (from the game data) and
templates.txt for the DLL (this script). The check runs both ways:
  - no unripe stage in templates.txt (the DLL would loot and destroy it);
  - every ripe stage that is harvested by an interaction (harvest refill, inventory or pickup) is in
    templates.txt or in EXCLUDED with a reason. Ripe stages harvested by hitting them with a tool
    (trees, cactus, artichoke ...) are never looted by ALP, wild or planted.

Input: tools/growth-stages.tsv, written by the EML export in tools/growth-export
(see MAINTAINING.md, "Growth stages"). Standard library only.

Usage:
  python tools/growth_stages.py            check both ways (exit 1 on any problem)
  python tools/growth_stages.py --list     every stage, by crop and level, with how it is harvested
  python tools/growth_stages.py --fix      remove the unripe stages from templates.txt and add the
                                           missing ripe refill stages as "plant"
  python tools/growth_stages.py --report BEFORE.txt [AFTER.txt]
      what mod.lua does with each growth stage, from auto-loot-plus-report.txt of an export run
      (emm run --export with the mod installed): before and after a change
"""
import argparse
import csv
import sys
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[1]
STAGES = ROOT / "tools" / "growth-stages.tsv"
TEMPLATES = ROOT / "mod" / "auto-loot-plus" / "templates.txt"

# How a stage is harvested (growth-export): by an interaction, which ALP can do, or by a tool hit.
BY_INTERACTION = {"refill", "inventory", "pickup"}

# Ripe stages harvested by an interaction that stay out of templates.txt on purpose.
EXCLUDED = {
    "882e8ae0-e0ac-4b08-9d17-7b1ce7909883":  # Z_T5_Plant_Growth_Edelweiss_2_harvest_DEPRECATED
        "deprecated template that no seedling places any more (mod.lua's junk patterns skip it too)",
}


class Stage(NamedTuple):
    name: str
    root: str
    level: str
    harvest: str
    destroy_on_loot: str
    yield_ids: str


def growth_stages(path: Path = STAGES) -> tuple[dict[str, Stage], dict[str, Stage]]:
    """(unripe, ripe): guid -> Stage for every growth level but the last, and for the last.

    Only chains whose root grows (keen::ecs::Growth): the base altar also has Upgrade levels.
    A template that is a middle level of any chain counts as unripe. The same as mod.lua's
    unripeStages and ripeStages."""
    unripe, ripe = {}, {}
    with path.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            if row["root_has_growth"] == "1":
                stage = Stage(row["stage_name"], row["root_name"], row["level"],
                              row["harvest"], row["destroy_on_loot"], row["yield"])
                (ripe if row["last"] == "1" else unripe)[row["stage_guid"]] = stage
    for guid in unripe:
        ripe.pop(guid, None)
    return unripe, ripe


def template_lines(path: Path = TEMPLATES) -> list[str]:
    return path.read_text(encoding="utf-8").splitlines(keepends=True)


def columns(line: str) -> list[str]:
    return [] if line.startswith("#") else line.rstrip("\r\n").split("\t")


def is_unripe(line: str, unripe: dict) -> bool:
    cols = columns(line)
    return bool(cols) and cols[0] in unripe


def listed(lines: list[str], unripe: dict) -> list[tuple[str, str]]:
    """(group, name) of each unripe stage templates.txt gives a group (the DLL would loot it)."""
    return sorted((cols[1], cols[3] if len(cols) > 3 else "")
                  for cols in map(columns, lines) if cols and cols[0] in unripe)


def missing_ripe(lines: list[str], ripe: dict[str, Stage]) -> dict[str, Stage]:
    """Ripe stages harvested by an interaction that templates.txt neither lists nor EXCLUDED explains."""
    known = {cols[0] for cols in map(columns, lines) if cols}
    return {guid: s for guid, s in ripe.items()
            if s.harvest in BY_INTERACTION and guid not in known and guid not in EXCLUDED}


def insert_sorted(lines: list[str], group: str, name: str, line: str) -> None:
    """Put line among its group, by name ignoring case, as the generated file is ordered."""
    at = len(lines)
    for i, cols in enumerate(map(columns, lines)):
        if len(cols) > 3 and (cols[1], cols[3].lower()) > (group, name.lower()):
            at = i
            break
    lines.insert(at, line)


def report_categories(path: Path, names: set[str]) -> dict[str, str]:
    """name -> mod.lua's categories for that name (report lines "category<TAB>name").

    The report has no GUIDs; templates sharing a name show all their categories joined by "+"."""
    out: dict[str, set[str]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        cols = line.split("\t")
        if len(cols) > 1 and cols[1] in names:
            out.setdefault(cols[1], set()).add(cols[0])
    return {name: "+".join(sorted(cats)) for name, cats in out.items()}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--report", nargs="+", type=Path, metavar="REPORT")
    args = ap.parse_args()
    unripe, ripe = growth_stages()
    print(f"{len(unripe)} unripe and {len(ripe)} ripe growth stages in {STAGES.name}")

    if args.list:
        stages = [(s, "ripe" if guid in ripe else "unripe", guid) for guid, s in {**unripe, **ripe}.items()]
        for s, kind, guid in sorted(stages, key=lambda t: (t[0].root, int(t[0].level))):
            print(f"  {s.root}\tL{s.level}\t{kind}\t{s.harvest}\t{s.name}\t{guid}")

    if args.report:
        names = {s.name for s in (*unripe.values(), *ripe.values())}
        cats = [report_categories(p, names) for p in args.report]
        print("mod.lua per growth stage: " + " -> ".join(p.name for p in args.report) + " (- = not handled)")
        for name in sorted(names):
            row = [c.get(name, "-") for c in cats]
            if any(r != "-" for r in row):
                print(f"  {' -> '.join(row):40s} {name}")

    lines = template_lines()
    hits = listed(lines, unripe)
    missing = missing_ripe(lines, ripe)
    if args.fix:
        # A refill stage stays standing and refills like the wild hazelnut and coffee bushes ("plant").
        # Any other missing ripe stage, or one without numeric item ids, needs a look: added by hand.
        added = {guid: s for guid, s in missing.items()
                 if s.harvest == "refill" and s.destroy_on_loot == "0"
                 and s.yield_ids and all(i.isdigit() for i in s.yield_ids.split(","))}
        if hits or added:
            lines = [l for l in lines if not is_unripe(l, unripe)]
            for group, name in hits:
                print(f"templates.txt: removed unripe stage {group}\t{name}")
            hits = []
            for guid, s in sorted(added.items(), key=lambda kv: kv[1].name.lower()):
                insert_sorted(lines, "plant", s.name, f"{guid}\tplant\t\t{s.name}\t{s.yield_ids}\n")
                print(f"templates.txt: added ripe refill stage plant\t{s.name}\t{s.yield_ids}")
                del missing[guid]
            TEMPLATES.write_bytes("".join(lines).encode("utf-8"))
    if hits:
        print(f"FAIL: templates.txt gives {len(hits)} unripe stages a group (the DLL loots them):")
        for group, name in hits:
            print(f"  {group}\t{name}")
    if missing:
        print(f"FAIL: {len(missing)} ripe stages are harvested by an interaction but neither in templates.txt"
              " nor in EXCLUDED (ALP leaves them alone):")
        for guid, s in sorted(missing.items(), key=lambda kv: kv[1].name):
            print(f"  {s.harvest}\t{s.name}\t{guid}")
    if hits or missing:
        return 1
    print("ok: no unripe stage in templates.txt; every ripe stage harvested by an interaction is listed"
          f" ({len(EXCLUDED)} excluded on purpose)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
