# Auto Loot Plus 1.1.8-fix2 (unofficial patch)

An unofficial patch that makes Auto Loot Plus 1.1.8 work with Enshrouded game version 1.0.1.0.
After that game update the mod loaded but did nothing; its log said "Muster 'box' gefunden: 0 mal".
It also makes looting work under Linux / Proton again (see "Linux / Steam Deck").
Only `dinput8.dll` changes; everything else is the original 1.1.8.

## Credit

Auto Loot Plus is made by Dellemann: https://www.nexusmods.com/enshrouded/mods/170
All credit for the mod goes to them; this repository only adapts it to the game update.
Please get the mod itself from the Nexus page, and report problems with this patch here, not to Dellemann.
This patch will be retired as soon as an official update is out.

## Install

1. Install Auto Loot Plus 1.1.8 from the Nexus page as usual, if you have not already.
2. Close the game.
3. Replace `dinput8.dll` in the Enshrouded game folder with the one from this release.
4. Keep the `mods\auto-loot-plus\` folder as it is.
5. EML (`dbghelp.dll`) is still needed for corpses, chests and the settings; without it only items on the ground are picked up.

To undo it, put the original 1.1.8 `dinput8.dll` back.

## Check that it works

Start the game and open `auto-loot-plus.log` in the game folder.
It should say "native 1.1.8-fix2" and then "Patches angewendet".
On any other game version it changes nothing and the log says why.

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
The "native 1.1.8-fix2" line in `auto-loot-plus.log` shows which one is active.

## Check the download

Each release lists the SHA-256 of `dinput8.dll` in `SHA256SUMS.txt`.
Compare it in PowerShell with `Get-FileHash dinput8.dll`.
The DLL is built by GitHub Actions from this source, and GitHub signs a record of that build.
To check it, run `gh attestation verify dinput8.dll --repo AKJama/auto-loot-plus-patch`.

## Build it yourself

Install Visual Studio Build Tools with "Desktop development with C++", plus git.
Run `build.cmd`; it fetches Dear ImGui and Vulkan-Headers at pinned versions and writes `build\dinput8.dll`.

## What changed

The first commit is the original 1.1.8 source as downloaded from the Nexus page.
The later commits add the fixes, the build and this README; each commit message explains its change.

- 1.1.8-fix2: looting works under Linux / Proton; the window pause trusts the mouse cursor only once the game has recentered it, plus the `windowCheck` setting (cause found by a Linux player on the Nexus page).
- 1.1.8-fix1: works with Enshrouded 1.0.1.0 (new code patterns, offsets read from the game code).

## Maintaining

After a game update, `python tools/check_exe.py` checks every pattern against the installed exe without starting the game.
How to fix, build and release is in `MAINTAINING.md`.

## Licence

MIT, as the original: see `LICENSE.txt` (Copyright (c) 2026 cw).
The DLL also contains Dear ImGui (MIT) and uses Vulkan-Headers (Apache 2.0); their licence texts come with each release.
