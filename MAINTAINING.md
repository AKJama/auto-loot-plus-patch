# Maintaining the patch

How to check, fix, build and release this patch after an Enshrouded update.
`native/main.cpp` (the `dinput8.dll`) depends on the game build.
The Lua part and the data files are the original 1.1.8, except the fix3 change for growth stages in `mod/auto-loot-plus/src/mod.lua` and `templates.txt` (below).

## After every game update

1. Close the game and let Steam finish the update.
2. Run the check against the installed exe (Python 3.10+, standard library only; it only reads the exe):

   ```
   python tools/check_exe.py
   ```

   Or pass a path: `python tools/check_exe.py "D:\SteamLibrary\steamapps\common\Enshrouded\enshrouded.exe"`.
3. "All checks pass." means the DLL will patch this build; it prints the build stamp to note in the release or README.
   Confirm in game once: `auto-loot-plus.log` must say `native 1.1.8-fixN` and then `Patches angewendet`, and walking over loot, a corpse and a chest must loot them.
4. Any `FAIL` line means the DLL will patch nothing (core pattern or constant) or turn a feature off (the lines under "Looked up at run time").
   The DLL never patches a build that fails a core check, so a broken build is safe, it just does nothing.

## What the check covers

The script reads every `kSig*` pattern and offset from `native/main.cpp` and replays `Patch()` and `PatchQuestFilter()` on the exe file:

- the seven core patterns, each found exactly once (four of them inside the 0x1000-byte magnet window that starts at `box`);
- the six constants of the value check (7.0, 0.5, 3.5, 1.75, 1.75, 3.5);
- the bytes the stubs rewrite (near site, the je, the check call, the helper calls, the loot-all block, the sight jne, the query call, the gate tail);
- the run-time lookups (`header`, `allocAccept`, the AcceptOffered context offset, `areaLoot`, `slotLoop`).

It cannot check the meaning of the code: a game change like the 1.0.1.0 switch from squared centre distance to surface distance passes a pattern but needs reading the code.
It also cannot check the context offsets that stay constants (`kCtxOfferRuntime` 0x488, the sight accessor 0x458 inside the `sight` pattern).
CI runs `python tools/check_exe.py --parse-only`, which fails when `main.cpp` and the script no longer agree on the names.

## Growth stages (fix3)

A planted seedling is a template with `keen::ecs::Growth`; its `keen::ecs::Upgrade` levels name the template it turns into at each level, and only the highest level is the ripe plant.
Many middle levels are wild-plant templates (the planted hazelnut's level 2 is the twig bush `Scatter_Loot_DeepWoods_Hazelnut_a_noharvest_InstantLoot`), so 1.1.8 looted young crops and destroyed them.
fix3 keeps every middle level away from the magnet in two places:

- `mod.lua` finds them in the game data at every start (`unripeStages`), releases none of them and sets `AreaLootOverride.exclude = true` on those that can be looted at all; the report line is `off:unripe`.
- `templates.txt` leaves them out, so the DLL treats them as unknown and never takes them, also without EML.

The ripe last levels are the other half.
Most are the same templates as wild plants or have their own `_harvest` template that 1.1.8 already listed (`farm` for the beds, `plant` for wetland and ancient-land crops).
Five are new kinds of template that 1.1.8 never released: the planted hazelnut, coffee, date, banana and coconut ripen into `*_Playerplanted_Refill` templates (family `Container`, no `DefaultInventory`, `keen::ecs::InteractionLootRefill`: the plant stays and its stock refills, 6 to 12 items, one every 180 s).
fix3 releases them as `plant` in `mod.lua` (`ripeStages`: a ripe level with `InteractionLootRefill`, an interaction offer and no `DestroyOnLoot`) and lists them in `templates.txt` as `plant` with their refill items as yield, the same group and yield as the wild hazelnut and coffee bushes.
Checked in game by hand on 2026-11-07 (farm hazelnuts, fix3 installed): the magnet's accept and loot-all harvest an `InteractionLootRefill` target, the ripe bushes stay standing, and young plants are left alone (1.1.8 never had such a target; farm animals use the same component and are not released).
Ripe levels harvested only by hitting them with a tool (`ResourceNodePickupDrops`, no inventory or offer: trees, cactus, yucca, bamboo, licuala palm, mangrove, artichoke, cabbage, aloe, `Bush_04_blooming_f`, `Bush_02_twigs_d_axe`) are not looted by ALP, wild or planted, and stay out.

`tools/growth-stages.tsv` lists every growth chain, from the offline EML export in `tools/growth-export` (same steps as for any EML export: a scratch folder with copies of `enshrouded.exe`, the original `enshrouded.kfc` (the install's `enshrouded.kfc.bak` once EML has run) and `enshrouded.kfc_resources`, the export mod in its `mods\`, then `emm run -g <scratch> --export`).
For each stage it also says how the game harvests it (`harvest`: refill, inventory, pickup, hit or none), whether looting removes it (`destroy_on_loot`) and the refill items (`yield`).
It was the same on build 1283531 (1.0.1.0) and 1287369 (4 November 2026 update): 65 chains, 214 middle levels, 65 ripe levels.

After a game update:

1. Rerun the export and copy `export\growth-stages.tsv` over `tools/growth-stages.tsv`.
2. `python tools/growth_stages.py` must say "ok"; it checks both ways: no middle level in `templates.txt`, and every ripe level harvested by an interaction (refill, inventory or pickup) in `templates.txt` or in its `EXCLUDED` list with a reason (only the deprecated Edelweiss today).
   `--fix` removes new middle levels and adds new ripe refill levels as `plant`; any other missing ripe level fails and is added by hand, in the group of its wild counterpart.
   `--list` prints every stage with how it is harvested.
3. To see what `mod.lua` does with each growth stage, run the export once with the original 1.1.8 mod and once with this one in the scratch `mods\` and compare the two `auto-loot-plus-report.txt`: `python tools/growth_stages.py --report before.txt after.txt`.
   On fix3 every middle level is `off:unripe` (39 changed from 1.1.8, 32 of them in `templates.txt`) and the five refill levels changed from `-` to `plant`; nothing else changed.

Known limits:

- `templates.txt` follows the game data only through the committed TSV: a new middle level after a game update is blocked by `mod.lua` at once, but the DLL alone (without EML) loots it until steps 1 and 2 are rerun.
- "Fell trees" (`smashTrees`, off by default) still tags young planted trees like grown ones; the aura never works near a base, so trees in a base are safe.
- Wild plants that are the same template as a middle level are no longer looted automatically either.

Reviews of fix3 (branch `fix3-unripe-crops`): `/code-review medium` had 9 findings, 5 fixed (ripe stage = highest level number, also in the export; pickup-only stages excluded too and `targetsOff` counts only real exclusions; README download check; the release ships the two files as a zip with their folders; `--report` keeps every category of a shared name), 4 not (the smash-trees and stale-TSV limits above, documented; the farm `_1_harvest` middle levels were never in `templates.txt`, so 1.1.8 did not harvest them either).
`/simplify` had 10 findings, 9 after dedup, 6 fixed (the growth stages found in the `genericRefs` pass instead of an extra pass over all templates; no `classify()` for a growth stage; the level number read once; the GUID string once in the loot-all pass; one line filter for the check and `--fix`; a comment that the 1.1.8 `seedling` name rule is covered by `lootAllBlocked`), 3 not (two more component scans for the 214 middle levels only; one export as the single source of the ripe-stage rule, a larger change; this review note itself, kept because the counts belong here).
The export after these fixes gave the same `growth-stages.tsv` and a byte-identical `auto-loot-plus-report.txt`.

Reviews of the ripe refill crops (commit 5468b30 and its fixes): `/code-review medium` had 10 findings, 6 fixed (the export counts a refill stage only with an interaction offer, as `mod.lua`; `--fix` adds a stage only with numeric item ids; the `mod.lua` rule skips quest and junk names like the other rules; `--fix` writes only when something changes; the component name in `T`; `is_unripe` through `columns()`), 4 not (CRLF and BOM in `templates.txt`: the file has neither; an empty refill stock retried every frame: only the game can tell, it is in the in-game check; `uv run` instead of `python` in this file: the tools are standard library only and CI runs `python`).
`/simplify` had 9 findings, 7 after dedup, 5 fixed (the refill rule moved into `lootAllCategory` next to the wild regrowing bushes; the export's stage looked up once and `destroy_on_loot` set in one place; `listed()` parses each line once; the `added` list only under `--fix`; no `ok` flag), 2 not (the export's own copy of `hasRealDefaultInventory`: a separate EML mod cannot share it; dropping the `ripeStages` gate: the cheat refill chests are `Container` with `InteractionLootRefill` too).
After both, the export gave the same `growth-stages.tsv` and `auto-loot-plus-report.txt` as commit 5468b30 (`plants=159`, `targetsOff=142`), and `--fix` on main's `templates.txt` gives the branch's file byte for byte.

### In-game check for fix3

Run it only when nobody is playing, through the rig in `enshrouded-chests` (`docs/test-rig.md`), never on the user's own world.
Growing a crop takes real game time, so the fixture is made once by hand:

1. In a rig world (not `world0`), plant two Hazelnut bush seedlings (item 2666875137) in a farm bed next to the Flame Altar, with "Bushes and plants" and "Harvest ripe crops" on and fix3 installed.
2. Wait until both are small leafy bushes with "Gather" (level 2; `Growth.minDuration` is 1200 s, *guess*: per level and in real time, shorter on good soil), log out, and keep the save as the fixture `alp-unripe-hazel`.
3. Scenario `alp-unripe-hazel`: set `debugLog=true` in the test copy's `config.txt`, load the world, stand at the bed for 15 s, log out.
4. Pass on fix3: the EML log says `[Auto Loot Plus] v1.1.8-fix3` with `targetsOff=142` (1 boss target and the 141 middle levels that can be looted at all); `auto-loot-plus.log` has no `Angenommen` line with `Vorlage=Scatter_Loot_DeepWoods_Hazelnut_a_noharvest_InstantLoot`; no Twig was added to the backpack; both bushes still stand (screenshot).
5. Control on fix2 (original `mod.lua` and `templates.txt`): the same scenario must fail: that `Angenommen` line, Twigs in the backpack, two empty spots with a dust puff.

The fixture world can also be checked by hand: walk up to a level-2 planted hazelnut with fix3 and the bush must stay.

Ripe refill crops, fixture `alp-ripe-hazel`: the same world once the bushes are ripe ("Harvest", nuts showing).
1. Scenario `alp-ripe-hazel`: `debugLog=true`, load, stand at the bed for 15 s, log out.
2. Pass on fix3: `auto-loot-plus.log` has an `Angenommen` line with `Vorlage=Scatter_Loot_DeepWoods_Hazelnut_d_nuts_harvest_Playerplanted_Refill`; hazelnuts (item 3360608010) were added to the backpack; both bushes still stand, without nuts (screenshot); the log does not repeat the accept every frame once the stock is empty.
3. Control on fix2: no `Angenommen` line for that Vorlage (`Vorlage unbekannt` instead), no hazelnuts, the nuts still on the bushes.
4. If fix3 does not harvest them either, the magnet cannot take `InteractionLootRefill` targets: take the five refill lines out of `templates.txt` and the `ripeStages` rule out of `mod.lua` again, and list them in `EXCLUDED`.

## Fixing a break

1. Find the old site in the previous exe (keep a copy of each exe you supported) and the same code in the new one.
   A register-only change shows as the same instruction with one ModRM or REX byte changed; replace that byte with `??` in the pattern.
   A moved offset inside a pattern means the pattern offsets (`kSig...` `size_t` constants) change.
2. Check whether the logic changed: compare the constants and how the magnet uses them, not just the bytes.
   If a constant changed its meaning, update the value check in `Patch()` together with the redirects; never drop the value check.
3. Bump `kModVersion` in `native/main.cpp` to the next `1.1.8-fixN` (and `VERSION` in `mod.lua` when the Lua or data changes).
4. Rerun `python tools/check_exe.py` on the new exe until every line is `ok`.
5. Build (below), install the DLL in a test copy of the game and check the log and the looting in game.
6. Add a line to "What changed" in `README.md` and update the first lines if the supported game version changed.

## Build

Install Visual Studio Build Tools with "Desktop development with C++", plus git.
Run `build.cmd`; it fetches Dear ImGui v1.91.9b and Vulkan-Headers v1.4.309 at pinned commits into `native\third_party` and writes `build\dinput8.dll`.

## Release

1. Commit the change on `main` with a message that says which game build it fixes and why.
2. Push `main`; CI builds the DLL.
3. Tag and push the tag: `git tag v1.1.8-fixN` then `git push origin v1.1.8-fixN`.
   The release workflow builds the DLL from the tag, writes `SHA256SUMS.txt`, attests the build and makes a draft release.
   `auto-loot-plus-mod-files.zip` comes from `python tools/mod_files_zip.py OUT.zip`: every file of `mod/auto-loot-plus` except the player's `config.txt`, under `mods/auto-loot-plus/`.
   The script reads the zip back and fails the build when `items.txt` or another file the DLL or the Lua reads is missing, `config.txt` is in it, or a file differs from the source; CI runs it too.
   Up to fix3 the zip held only `mod.lua` and `templates.txt`, so players without the original mod files had no `items.txt` and an empty "Ignore items" list.
4. Check the draft (the DLL's `native` line in a game log, the SHA-256), write the notes, then publish it by hand.
5. Tell players on the Nexus page of Auto Loot Plus (mod 170) in one short comment with the release link.

## When the original author updates

If Dellemann releases an official update that works with the current game, say so at the top of `README.md`, point to the Nexus page and stop releasing fixes.
