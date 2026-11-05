# Maintaining the patch

How to check, fix, build and release this patch after an Enshrouded update.
Only `native/main.cpp` (the `dinput8.dll`) depends on the game build; the Lua part and the data files are the original 1.1.8.

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

## Fixing a break

1. Find the old site in the previous exe (keep a copy of each exe you supported) and the same code in the new one.
   A register-only change shows as the same instruction with one ModRM or REX byte changed; replace that byte with `??` in the pattern.
   A moved offset inside a pattern means the pattern offsets (`kSig...` `size_t` constants) change.
2. Check whether the logic changed: compare the constants and how the magnet uses them, not just the bytes.
   If a constant changed its meaning, update the value check in `Patch()` together with the redirects; never drop the value check.
3. Bump `kModVersion` in `native/main.cpp` to the next `1.1.8-fixN`.
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
4. Check the draft (the DLL's `native` line in a game log, the SHA-256), write the notes, then publish it by hand.
5. Tell players on the Nexus page of Auto Loot Plus (mod 170) in one short comment with the release link.

## When the original author updates

If Dellemann releases an official update that works with the current game, say so at the top of `README.md`, point to the Nexus page and stop releasing fixes.
