# UNBSE mod-manager compatibility — implementation brief

Companion to `docs/mod-manager-compatibility-study.md` (full evidence and
citations). This file is the hand-off for implementation work: what is broken,
why, and what to build. Read the study for any "why" you need to verify.

## Context

- UNBSE `0.11.0-rc.1` = UE4SS proxy `dwmapi.dll` + pinned UE4SS runtime
  (`ue4ss/UE4SS.dll`, commit `68dd45cb`, patched via `ue4ss/patches/*`) + two
  DLL-only UE4SS C++ mods: `ue4ss/Mods/UNBSE` and
  `ue4ss/Mods/UNBSEOBSE64Interop`, each enabled by a 0-byte `enabled.txt`.
  The release archive ships no `mods.txt` and no `mods.json`.
- Build/package entry points: `ue4ss/scripts/Build-UNBSEUE4SSMod.ps1`,
  `ue4ss/scripts/Package-UNBSERelease.ps1` (assembles the archives from
  `out/ue4ss-package` + the foundation zip; `ue4ss/foundation-manifest.json`
  lists `requiredRuntimeArtifacts` and pins script hashes —
  `Set-UNBSECoreManifestBoundary.ps1` / `Update-UNBSESourceFilePins.ps1`
  must be re-run when pinned files change).
- Mod managers involved: Mod Organizer 2 2.5.3 beta with the Oblivion
  Remastered plugin from `modorganizer-basic_games`
  (`games/game_oblivion_remaster.py`, `games/oblivion_remaster/*`), and Vortex
  with the "Oblivion Remastered Vortex Support" extension 0.1.8
  (`Nexus-Mods/game-oblivionremastered`).

## Findings (condensed)

F1. **MO2 cannot load a virtual `dwmapi.dll`.** USVFS installs its hooks by
    hijacking the suspended main thread's entry point, after the Windows
    loader has resolved the exe's static imports. A proxy DLL that only exists
    in an MO2 mod folder is never loaded. MO2's plugin diagnoses `dwmapi.dll`
    and offers: delete it and use the "OBSE64 ue4ss Loader" OBSE plugin
    (dead for UNBSE — UNBSE requires OBSE64 removed), or rename it in the real
    folder (`ue4ss_loader.dll`) and force-load it (works: the proxy's system
    DLL name is compiled in, and it finds `ue4ss/UE4SS.dll` relative to
    `GetModuleFileNameW(hInstDll)`).

F2. **MO2 deletes `enabled.txt` and owns `mods.txt`.** The plugin's UE4SS tab
    (`oblivion_remaster/ue4ss/widget.py`, `_parse_mod_files`,
    `update_mod_files`) unlinks every `enabled.txt` it finds in MO2 mods *and*
    in the real `Win64\ue4ss\Mods`, lists only folders containing
    `scripts/main.lua`, and overlays the profile's `mods.txt` + `mods.json`
    onto `Win64\ue4ss\Mods` (`mappings()` in `game_oblivion_remaster.py`).
    UNBSE's DLL-only mods are never listed and lose their markers → UE4SS
    never starts them, even when UE4SS itself loads (F1 workaround). This is
    the reason the reporter's rename+force-load attempt fails.

F3. **MO2 installer routing.** `mod_data_checker.py` `fix()` moves any archive
    containing `ue4ss/UE4SS.dll` (or `OblivionRemastered/Binaries/Win64/ue4ss/UE4SS.dll`)
    wholesale to `Root/OblivionRemastered/Binaries/Win64/`, which only Root
    Builder honours (Copy/Link mode required for DLLs; USVFS mode fails per
    F1). A mods-only archive without `scripts/main.lua` is INVALID. Native
    mappings are `Data/`, `Paks/`, `OBSE/` (→ `Win64/OBSE`), `UE4SS/`
    (→ `Win64/ue4ss/Mods`), `Movies/`, `GameSettings/`.

F4. **Vortex has no VFS problem.** Hard links into the real folder; the proxy
    loads normally. Vortex classifies any archive containing
    `UE4SS-settings.ini` as "the UE4SS injector" (`testUE4SSInjector`,
    priority 10) and re-roots it at the depth of `dwmapi.dll` under
    `OblivionRemastered/Binaries/Win64/` — layout is preserved. Files with no
    extension (`ue4ss/LICENSE`) are dropped. Never ship `UE4SS-settings.ini`
    without `dwmapi.dll` at the same depth (files get flattened into `Win64`).

F5. **Vortex enablement is fine but its bookkeeping is inert.** Vortex writes
    only `mods.json` (for Lua-type mods); the pinned UE4SS never reads
    `mods.json` (grep is empty). `ignoreDeploy: ['ENABLED.TXT', ...]` only
    matches a root-level file (Vortex core `BlacklistSet` uses plain
    `minimatch`, no `matchBase`), so nested `ue4ss/Mods/UNBSE/enabled.txt`
    deploys. Unverified at runtime — check the file exists after deploy.

F6. **Vortex breakers.** (a) The extension auto-installs OBSE64 (Nexus 282) as
    a Steam requirement and sets `obse64_loader.exe` as the primary launch
    tool → OBSE64 and UNBSE's interop both load `OBSE/Plugins`. (b)
    `testBluePrintModManager` finds the mod containing `UE4SS-settings.ini`
    (UNBSE) and, because `ue4ss/Mods/BPModLoaderMod` is missing, raises a
    non-dismissable "UE4SS missing or corrupted" warning after every deploy.
    (c) If Nexus "UE4SS for OblivionRemastered" (mod 32) is also installed,
    `dwmapi.dll`/`UE4SS.dll`/`UE4SS-settings.ini` conflict; Vortex refuses to
    launch until a rule exists, and if mod 32 wins UNBSE's `main.dll` runs on
    a foreign `UE4SS.dll`.

F7. **UNBSE and the pinned UE4SS are VFS-clean.** No `canonical`,
    `GetFinalPathNameByHandle`, reparse checks, or path comparisons;
    `LoadLibraryExW` + `std::filesystem` only (all USVFS-hooked). No runtime
    code change is needed to tolerate a VFS. (`weakly_canonical` exists in
    UE4SS only for `ModsFolderPath`/`ControllingModsTxt`/`UE4SS_MODS_PATHS`
    overrides, which UNBSE does not set.)

## Work items

Do W1–W3 first; they fix the reported MO2 case and the Vortex nag with no
C++ changes. W4–W6 are follow-ups.

### W1. Lua stub in both UNBSE mod folders

Add `Scripts/main.lua` to `ue4ss/Mods/UNBSE/` and
`ue4ss/Mods/UNBSEOBSE64Interop/` in the packaged output. Content: a
comment-only file explaining it exists so mod managers recognise the folder
as a UE4SS mod. Keep `enabled.txt`.

- Where: the core package build (`Build-UNBSEUE4SSMod.ps1` /
  `UNBSEUE4SSMod.psm1` — wherever `enabled.txt` is emitted into
  `out/ue4ss-package`) and `Package-UNBSERelease.ps1` artifact lists;
  `foundation-manifest.json` package artifact list if it enumerates files.
- Why it works: MO2 lists folders with `scripts/main.lua` and writes them to
  `mods.txt` (`Name : 1`); UE4SS starts C++ and Lua mods by the same name
  (`UE4SSProgram.cpp` `start_mods<CppMod>` and `<LuaMod>`). Vortex's Lua
  installer (`testLuaMod`) classifies a mods-only archive as a Lua mod and
  deploys `Mods/<folder>/...` under `Win64/ue4ss`.
- Acceptance: `UE4SS.log` shows `Starting C++ mod 'UNBSE'` when the profile
  `mods.txt` contains `UNBSE : 1` and no `enabled.txt` exists; MO2's
  `OblivionRemasteredModDataChecker.dataLooksValid` returns FIXABLE/VALID for
  the mods archive (`ue4ss/Mods/<name>/scripts/main.lua` present); an empty
  Lua mod causes no errors in `UE4SS.log`.

### W2. Split the release archives

Produce three archives from `Package-UNBSERelease.ps1`:

1. `UNBSE-<ver>-runtime.zip` — `dwmapi.dll`, `ue4ss/UE4SS.dll`,
   `ue4ss/UE4SS-settings.ini`, `ue4ss/LICENSE.txt` (renamed so Vortex keeps
   it; update the manifest's `requiredRuntimeArtifacts`), plus
   `ue4ss/Mods/BPModLoaderMod/` and `ue4ss/Mods/BPML_GenericFunctions/` taken
   from the pinned UE4SS `assets/Mods` (silences Vortex F6b; standard in
   every UE4SS install), plus `ue4ss/Mods/mods.json` containing `[]`.
   Must include `dwmapi.dll` (F4 flattening rule).
2. `UNBSE-<ver>-mods.zip` — `ue4ss/Mods/UNBSE/...` and
   `ue4ss/Mods/UNBSEOBSE64Interop/...` including W1 stubs and `enabled.txt`.
   Must not contain `UE4SS-settings.ini` or `UE4SS.dll`.
3. Keep `UNBSE-<ver>.zip` (drop-in = 1 + 2 merged) for manual installs.
   Drop `-game-root.zip` (MO2 reroutes it to `Root/` exactly like the flat
   one; Vortex re-roots at `dwmapi.dll` anyway) unless there is a non-manager
   reason to keep it.

- Update `UNBSE-SHA256SUMS.txt` generation and `release/SHA256SUMS.txt` for
  the new set; update `Set-UNBSECoreManifestBoundary.ps1` /
  `Update-UNBSESourceFilePins.ps1` pins for the changed scripts.
- Acceptance: each archive's listing matches the above; Vortex's
  `installUE4SSInjector` logic applied to archive 1 yields
  `OblivionRemastered/Binaries/Win64/...` for every file; archive 2 contains
  `.lua` files and no `UE4SS-settings.ini`.

### W3. Documentation

`README.md` "Install" and the Nexus page text (kept under ignored `local/`):

- Manual: unchanged (drop-in zip).
- MO2: install `-runtime.zip` into the real
  `OblivionRemastered\Binaries\Win64`; accept MO2's guided fix (renames
  `dwmapi.dll` → `ue4ss_loader.dll`) and add it under Force Load Libraries
  for `OblivionRemastered-Win64-Shipping.exe` (absolute path); install
  `-mods.zip` through MO2 (auto-fixed to `UE4SS/UNBSE`, `UE4SS/UNBSEOBSE64Interop`);
  tick both in the UE4SS Mods tab. Root Builder optional, Copy/Link mode only.
  `UE4SS.log` may land in Overwrite.
- Vortex: install `-runtime.zip` and `-mods.zip` as mods; if Vortex installed
  the OBSE64 requirement, disable it and set the primary tool back to Steam;
  if Nexus UE4SS (mod 32) is installed, uninstall it or resolve the conflict
  in UNBSE's favour.
- State the OBSE64-removal prerequisite in manager terms.

### W4. Interop stand-down when a legacy OBSE64 runtime is loaded

`ue4ss/mod/OBSE64Interop/src/dllmain.cpp`: before scanning `OBSE/Plugins`,
check for an already-loaded `obse64_*.dll` / `obse64_steam_loader.dll`
(`GetModuleHandleW` by known name, or enumerate modules and match the
prefix). If present, emit a structured event
(`{"schema":"UNBSE.OBSE64Interop","event":"runtime-conflict",...}`), skip
plugin loading and hook installation. Prevents the double-load crash when a
manager launches through `obse64_loader.exe` (F6a).

- Acceptance: with `obse64_1_512_105.dll` in the process, `UE4SS.log` shows
  the conflict event and zero `load` events from the interop; without it,
  behaviour is unchanged (existing fixture logs still match).

### W5. Foundation patch: `mods.json` as an enable source

New patch under `ue4ss/patches/` on top of `68dd45cb`: in
`UE4SS/src/UE4SSProgram.cpp` `start_mods<ModType>()`, after the `mods.txt`
pass and before the `enabled.txt` pass, parse `<mods dir>/mods.json` if
present — an array of `{"mod_name": string, "mod_enabled": bool}` (the shape
both MO2 and Vortex write) — and start listed, enabled, not-yet-started mods
by name (same lookup as the `mods.txt` branch). Optionally treat a per-mod
`unbse-mod-manifest.json` as an enable marker in the `enabled.txt` pass.
Keep the change small and self-contained; update `foundation-manifest.json`
(patch list, source hashes) and rebuild.

- Acceptance: a `mods.json` with `{"mod_name":"UNBSE","mod_enabled":true}` and
  no `enabled.txt`/`mods.txt` entry starts the mod; malformed JSON logs a
  warning and does not abort; existing behaviour without `mods.json` is
  byte-for-byte identical in `UE4SS.log`.

### W6. `UNBSELoader.exe` (MO2-native launcher)

Reuse the retained `src/Loader.cpp` pattern (`CreateProcessW` with
`CREATE_SUSPENDED`, write a bootstrap thunk, `CreateRemoteThread` →
`LoadLibraryW`) to inject `ue4ss\UE4SS.dll` directly, bypassing `dwmapi.dll`.
Ship next to the exe in `-runtime.zip`; MO2 users add it as an executable
(same workflow as `obse64_loader.exe`/`skse64_loader.exe`).

- Verify before shipping: USVFS's `CreateProcessInternalW` hook injects into
  the child before our remote thread runs; UE4SS initialises correctly when
  its `DllMain` runs from a remote thread instead of import time; Steam
  relaunch behaviour (`steam_appid.txt` is already present); the proxy must
  not also be loaded (document: do not keep `dwmapi.dll` when using the
  loader, or make the loader pass `--disable-ue4ss`-equivalent).
- Acceptance: launched from MO2 with UNBSE mods in an MO2 mod folder,
  `UE4SS.log` shows `Loading mods from: ...\Win64\ue4ss\Mods` and both UNBSE
  mods starting; the same loader works outside MO2.

## Verification checklist for testers (put in the Nexus post / README)

1. `Win64\ue4ss\UE4SS.log` exists (MO2: also check Overwrite). If absent,
   UE4SS never loaded (F1).
2. Log contains `Loading mods from:` but no `[UNBSE.Core]` line → mods not
   enabled (F2). Check `ue4ss\Mods\UNBSE\enabled.txt` presence and the
   `mods.txt` content that UE4SS reports.
3. Vortex: confirm `obse64_loader.exe` is not the primary tool and no
   `obse64_*.dll` is in `Win64`; confirm no file conflicts on `UE4SS.dll`.

## Do not

- Ship `ue4ss/Mods/mods.txt` in the drop-in (overwrites a manual user's load
  order).
- Depend on MO2's `ue4ss_use_root_builder` setting (opt-in; still deletes
  `enabled.txt`).
- Publish any archive containing `UE4SS-settings.ini` but no `dwmapi.dll`.
- Add path canonicalisation (`std::filesystem::canonical`,
  `GetFinalPathNameByHandle`) or reparse-point checks to runtime code.
