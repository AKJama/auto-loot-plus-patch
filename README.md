# Auto Loot Plus 1.1.8-fix4 (unofficial patch)

An unofficial patch that makes Auto Loot Plus 1.1.8 work with Enshrouded game version 1.0.1.0.
After that game update the mod loaded but did nothing; its log said "Muster 'box' gefunden: 0 mal".
It also makes looting work under Linux / Proton again (see "Linux / Steam Deck").
fix3 stops it from harvesting planted crops before they are ripe, and lets it harvest ripe planted hazelnut, coffee, dates, bananas and coconuts (see "Planted crops").
fix4 ships every mod file the patch reads with the download, so "Ignore items" works without the original mod files.
fix1 and fix2 change only `dinput8.dll`; fix3 changes only `mods\auto-loot-plus\src\mod.lua` and `mods\auto-loot-plus\templates.txt`; the other mod files are the original 1.1.8.

## Credit

Auto Loot Plus is made by Dellemann: https://www.nexusmods.com/enshrouded/mods/170
All credit for the mod goes to them; this repository only adapts it to the game update.
Please get the mod itself from the Nexus page, and report problems with this patch here, not to Dellemann.
This patch will be retired as soon as an official update is out.

## Install

1. Install Auto Loot Plus 1.1.8 from the Nexus page as usual, if you have not already.
2. Close the game.
3. Replace `dinput8.dll` in the Enshrouded game folder with the one from this release.
4. Unpack `auto-loot-plus-mod-files.zip` from this release into the Enshrouded game folder and let it overwrite.
   It holds every file of `mods\auto-loot-plus` except `config.txt`: `items.txt` (the item list of "Ignore items"), `quest_items.txt`, `templates.txt`, `mod.json`, `src\mod.lua` and `src\autosmash_program.lua`.
   Your settings in `mods\auto-loot-plus\config.txt` are kept.
5. EML (`dbghelp.dll`) is still needed for corpses, chests and the settings; without it only items on the ground are picked up.

To undo it, put the original 1.1.8 files back.

## Check that it works

Start the game and open `auto-loot-plus.log` in the game folder.
It should say "native 1.1.8-fix4", then "Patches angewendet" and "items.txt: 4262 Gegenstaende".
If it says "items.txt missing or empty", the "Ignore items" list has no suggestions: unpack `auto-loot-plus-mod-files.zip` into the game folder again.
On any other game version it changes nothing and the log says why.
The day's EML log in `logs\` says "[Auto Loot Plus] v1.1.8-fix4" and `targetsOff=142` (1 in 1.1.8; on the 4 November 2026 build).

Tested on Enshrouded 1.0.1.0 (2 November 2026, exe build stamp `6ae4601e`) and on the update of 4 November 2026 (Steam build 26430625, exe build stamp `6aeb144c`), which needed no change.

## Linux / Steam Deck

Under Proton, Steam uses its own built-in `dinput8.dll` and `dbghelp.dll` and ignores the ones in the game folder, so the mod never starts and no `auto-loot-plus.log` appears.
Tell Proton to use the mod's DLLs: in Steam, open Enshrouded > Properties > Launch Options and enter

```
WINEDLLOVERRIDES="dinput8=n,b;dbghelp=n,b" %command%
```

`dinput8` is for Auto Loot Plus and `dbghelp` is for EML; without EML you only need `WINEDLLOVERRIDES="dinput8=n,b" %command%`.
This launch option is still needed with fix2.

Up to fix1, the mod loaded under Proton (F8 worked, smashing worked) but looted nothing, and it even blocked picking up with E held.
The mod pauses while a game window such as the inventory is open, and it tells that from the mouse cursor being shown.
Under Proton the cursor counts as shown all the time, so the mod thought a window was always open.
fix2 trusts the cursor only once the game has recentered it during play, which it does on Windows and never under Proton.
Under Proton the mod therefore no longer pauses for game windows; it still pauses while its own F8 menu is open.
A Linux player on the Nexus page found the cause and confirmed it with their own build; thank you.
fix2 itself was checked under Wine 10.0 with an offline load test, not yet in the real game under Proton; please report whether it works for you.

If looting still stops or misbehaves around game windows, add one line to `mods\auto-loot-plus\config.txt`:
`windowCheck=auto` is the default described above, `windowCheck=on` always trusts the cursor (as in 1.1.8), and `windowCheck=off` never pauses for game windows.
The "native 1.1.8-fixN" line in `auto-loot-plus.log` shows which one is active.

## Planted crops

Up to fix2, Auto Loot Plus harvested some planted crops while they were still growing, which destroyed them.
A planted hazelnut bush, for example, gave a few twigs and left an empty bed.
A planted seedling grows through a row of stages, and many middle stages are the same objects as wild plants: the young hazelnut bush is the wild twig bush that "Bushes and plants" harvests.
fix3 never loots a middle stage of any crop on its own; only the ripe last stage is harvested, as before.
In 1.1.8 that hit 32 stages of hazelnut, coffee, elderberry ("Butterfly Lover Bush"), pumpkin, yellow fog mushroom, shiitake, taro, ginger, algae, daylily, papyrus, reed, cattail and the steppe twig bush.
Wild plants that are the same object as such a young stage are no longer looted automatically either; press E on them as usual.

The other half: ripe planted hazelnut and coffee bushes and ripe date, banana and coconut palms were never harvested automatically.
They ripen into objects of their own that refill their nuts or fruit over time, and 1.1.8 did not know them.
fix3 harvests them with "Bushes and plants", like the wild hazelnut and coffee bushes; they stay standing and refill as usual.
Crops you harvest by hitting them with a tool (trees, cactus, yucca, bamboo, licuala palm, mangrove, artichoke, cabbage, aloe, flowering and steppe twig bushes) stay manual, planted or wild.

Until fix3 is installed, switch off "Bushes and plants" in the F8 menu (`collectPlants=false`) to keep planted hazelnut, coffee, elderberry and the wetland crops safe.
That also stops harvesting wild berry bushes, twig bushes, herbs, plant fiber, reeds and fireflies, and the ripe wheat, wetland and ancient-land crops.
Planted pumpkins and yellow fog mushrooms are ground loot: only "Ground loot" off protects them, and that stops picking up everything on the ground.

## Check the download

Each release lists the SHA-256 of `dinput8.dll` (and, from fix3, of `auto-loot-plus-mod-files.zip`) in `SHA256SUMS.txt`.
Compare it in PowerShell with `Get-FileHash dinput8.dll` and `Get-FileHash auto-loot-plus-mod-files.zip`.
Both are built by GitHub Actions from this source, and GitHub signs a record of that build.
To check it, run `gh attestation verify dinput8.dll --repo AKJama/auto-loot-plus-patch`, and the same for the zip.

## Build it yourself

Install Visual Studio Build Tools with "Desktop development with C++", plus git.
Run `build.cmd`; it fetches Dear ImGui and Vulkan-Headers at pinned versions and writes `build\dinput8.dll`.

## What changed

The first commit is the original 1.1.8 source as downloaded from the Nexus page.
The later commits add the fixes, the build and this README; each commit message explains its change.

- 1.1.8-fix4: the download includes every mod file except `config.txt`, `items.txt` too, so "Ignore items" works without the original mod files; a missing `items.txt`, `quest_items.txt` or `templates.txt` is named in the log in English with the fix, and a missing `items.txt` also in the F8 menu.
- 1.1.8-fix3: planted crops are no longer harvested before they are ripe, and ripe planted hazelnut, coffee, dates, bananas and coconuts are harvested (`mod.lua` and `templates.txt`; the DLL stays fix2's).
- 1.1.8-fix2: looting works under Linux / Proton; the window pause trusts the mouse cursor only once the game has recentered it, plus the `windowCheck` setting (cause found by a Linux player on the Nexus page).
- 1.1.8-fix1: works with Enshrouded 1.0.1.0 (new code patterns, offsets read from the game code).

## Maintaining

After a game update, `python tools/check_exe.py` checks every pattern against the installed exe without starting the game.
How to fix, build and release is in `MAINTAINING.md`.

## Licence

MIT, as the original: see `LICENSE.txt` (Copyright (c) 2026 cw).
The DLL also contains Dear ImGui (MIT) and uses Vulkan-Headers (Apache 2.0); their licence texts come with each release.
