# Auto Loot Plus 1.1.8-fix1 (unofficial patch)

An unofficial patch that makes Auto Loot Plus 1.1.8 work with Enshrouded game version 1.0.1.0.
After that game update the mod loaded but did nothing; its log said "Muster 'box' gefunden: 0 mal".
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
It should say "native 1.1.8-fix1" and then "Patches angewendet".
On any other game version it changes nothing and the log says why.

## Linux / Steam Deck

Under Proton, Steam uses its own built-in `dinput8.dll` and `dbghelp.dll` and ignores the ones in the game folder, so the mod never starts and no `auto-loot-plus.log` appears.
Tell Proton to use the mod's DLLs: in Steam, open Enshrouded > Properties > Launch Options and enter

```
WINEDLLOVERRIDES="dinput8=n,b;dbghelp=n,b" %command%
```

`dinput8` is for Auto Loot Plus and `dbghelp` is for EML; without EML you only need `WINEDLLOVERRIDES="dinput8=n,b" %command%`.
This was checked under Wine 10.0 with an offline load test, not yet in the real game under Proton; please report whether it works for you.

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
The second commit is the whole fix; its message explains each change.

## Licence

MIT, as the original: see `LICENSE.txt` (Copyright (c) 2026 cw).
The DLL also contains Dear ImGui (MIT) and uses Vulkan-Headers (Apache 2.0); their licence texts come with each release.
