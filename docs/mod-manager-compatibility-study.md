# UNBSE under Mod Organizer 2 and Vortex — compatibility study

Status: study / review, 2026-08-30. Target: UNBSE `0.11.0-rc.1` (pinned UE4SS
`68dd45cb`, v3.0.1-1008) on the Steam build `1.512.105.0`.

Scope: explain why UNBSE does not work when installed or launched through Mod
Organizer 2 (MO2 2.5.3 beta, Oblivion Remastered plugin from
`modorganizer-basic_games`) or Vortex (extension "Oblivion Remastered Vortex
Support" 0.1.8), and propose fixes. Nothing in this document changes the
release; it is the input for the follow-up work items in section 7.

Current community-package verification (Nexus mod 32, official API queried
2026-08-30): the sole current MAIN file was file `14056`, `UE4SS` version
`3.0.1.a`, archive SHA-256
`8D805AFDB4C8293432B87C5BD1A10ED9D80A3E576C8DD4A38AE22CA0050A86A9`.
It identifies its runtime as UE4SS `3.0.1-394-g437a8ff`, older than UNBSE's
pinned `3.0.1-1008-g68dd45cb`. Its useful familiar layout is the root proxy plus
`ue4ss/`, `BPML_GenericFunctions`, `BPModLoaderMod`, and shared Lua helpers. It
ships `mods.txt` with the two Blueprint modules enabled, not `mods.json`, and it
ships neither `MemberVariableLayout.ini` nor an extensionless license. UNBSE
therefore adopts only the standard module/layout convention from its newer
pinned foundation; it does not copy the community DLLs, PDB, manager-owned mod
list, or older settings.

Report that triggered this study (MO2 2.5.3b12, Steam):

> the shipped dwmapi.dll with your mod does not work within MO2, the
> "OBSE64 ue4ss Loader" mod circumvents the documented workarounds for MO2 with
> the dwmapi.dll hidden, this of course would stop your mod from loading. The
> MO2 workaround is to rename dwmapi.dll to say ue4ss_loader.dll (and force
> preload of the DLL in the app setting within MO2), but of course again that
> stops your mod from working.

## 1. Summary

The user's theory ("virtual locations break the loader") is correct for one of
the failures, but it is not the failure that actually stops UNBSE. Three
independent problems stack up:

1. **Loader injection (MO2 only).** MO2's USVFS cannot make a virtual
   `dwmapi.dll` visible to the Windows loader, because the game's import table
   is resolved before USVFS is injected. MO2's own Oblivion Remastered plugin
   flags `dwmapi.dll` as a problem and documents two workarounds: (a) delete it
   and load UE4SS through the "OBSE64 UE4SS Loader" OBSE plugin, or (b) rename
   it (`ue4ss_loader.dll`) and force-load it. Workaround (a) is dead for UNBSE
   because UNBSE requires the OBSE64 loader/runtime to be removed. Workaround
   (b) does load UE4SS — but see problem 2.

2. **Mod enablement (MO2, definite; Vortex, mostly fine).** UNBSE's two UE4SS
   mods (`UNBSE`, `UNBSEOBSE64Interop`) are DLL-only C++ mods enabled by a
   per-mod `enabled.txt`. Both managers assume "UE4SS mod = folder with
   `Scripts/main.lua`". MO2's plugin **deletes every `enabled.txt` it finds** —
   in MO2 mods *and* in the real `Win64\ue4ss\Mods` — and overlays its own
   profile `mods.txt`/`mods.json` onto `ue4ss\Mods`, listing only Lua mods.
   Result: even with a working UE4SS, UNBSE's mods are never started. This is
   the reason the reporter's rename-and-force-load attempt "stops your mod from
   working". Vortex only manages `mods.json` (which the pinned UE4SS build does
   not read) and, by code reading, does deploy nested `enabled.txt`, so
   enablement survives under Vortex.

3. **Archive classification and ecosystem assumptions (both).** The drop-in
   archive is neither an MO2 "UE4SS mod" nor a Vortex "Lua mod":
   - MO2's installer moves anything containing `ue4ss/UE4SS.dll` to
     `Root/OblivionRemastered/Binaries/Win64/`, which is inert unless the Root
     Builder plugin is installed in Copy or Link mode (USVFS mode has the
     problem-1 limitation for DLLs). The new `-game-root.zip` layout is handled
     the same way. A mods-only archive without a `Scripts/main.lua` is
     classified INVALID.
   - Vortex classifies any archive containing `UE4SS-settings.ini` as "the
     UE4SS injector" (correct placement), then permanently nags "UE4SS missing
     or corrupted" because UNBSE ships no `ue4ss/Mods/BPModLoaderMod`, and it
     conflicts with the Nexus "UE4SS for OblivionRemastered" mod if that is
     already installed (unresolved conflicts block launching from Vortex).
   - Vortex auto-installs OBSE64 as a Steam "requirement" and makes
     `obse64_loader.exe` the primary launch tool; UNBSE's install instructions
     require removing OBSE64. If both stay, OBSE64 and UNBSE's interop load
     `OBSE/Plugins` twice.
- The managers' tooling assumes `mods.json` and `BPModLoaderMod`; the current
  community UE4SS package supplies the Blueprint loader and its own `mods.txt`,
  while UNBSE's original archive supplied neither convention.

UNBSE's own C++ code and the pinned UE4SS runtime are clean with respect to
virtual paths: no canonicalisation, no reparse-point checks, no path
comparisons; DLLs are loaded through `LoadLibraryExW`, directories through
`std::filesystem`, all of which USVFS hooks. Nothing in UNBSE needs to change
to *tolerate* a VFS. What has to change is packaging, the enable marker, and
the loader story (section 7).

## 2. How the pieces load today

Drop-in layout (`release/UNBSE-0.11.0-rc.1.zip`, extracted into `Win64`):

```
dwmapi.dll                                   UE4SS proxy (import-table hijack)
ue4ss/UE4SS.dll, UE4SS-settings.ini, LICENSE UE4SS runtime (pinned, patched)
ue4ss/Mods/UNBSE/dlls/main.dll               C++ mod
ue4ss/Mods/UNBSE/enabled.txt                 enable marker (0 bytes)
ue4ss/Mods/UNBSE/sdk/*.h, unbse-mod-manifest.json
ue4ss/Mods/UNBSEOBSE64Interop/dlls/main.dll  C++ mod
ue4ss/Mods/UNBSEOBSE64Interop/enabled.txt
UNBSE-README.md, UNBSE-SHA256SUMS.txt
```

There is no `mods.txt` and no `mods.json` in the archive; the two mods rely
entirely on `enabled.txt` (`Package-UNBSERelease.ps1` copies the markers from
the build output; nothing generates a `mods.txt`).

Load chain, with the path each stage uses (UE4SS source at
`out/ue4ss-source`, commit `68dd45cb`):

| Stage | Code | How the path is found |
|---|---|---|
| Game exe imports `dwmapi.dll` | Windows loader | exe directory, then System32 |
| Proxy `DllMain` | `UE4SS/proxy_generator/main.cpp` → generated `load_ue4ss_dll` | `GetModuleFileNameW(hInstDll)`, then `<proxy dir>/ue4ss/UE4SS.dll`; `--ue4ss-path` argument and `override.txt` are honoured; fallback `LoadLibrary(L"UE4SS.dll")`; failure → MessageBox + `ExitProcess` |
| Real system DLL | same file, `load_original_dll` | `GetSystemDirectory() + "\\dwmapi.dll"` — the name is compiled in, so **renaming the proxy is safe** |
| UE4SS root | `UE4SS/src/UE4SSProgram.cpp:454-507` `setup_paths` | `GetModuleFileNameW(hModule of UE4SS.dll)` → `m_root_directory` = its parent; `Mods` = `<root>/Mods` (`setup_mod_directory_path`, :516-539); game dir from `GetModuleFileNameW(NULL)` |
| Mod discovery | `UE4SSProgram.cpp:1268-1316` `setup_mods` | `std::filesystem::directory_iterator(<root>/Mods)`; a folder with `dlls/` becomes a `CppMod`, with `scripts/` a `LuaMod` |
| Enablement | `UE4SSProgram.cpp:1446-1611` `start_mods` | Part 1: every `mods.txt` (`Name : 1`) by mod *name*; Part 2: any mod folder with `enabled.txt`. **No `mods.json` support anywhere in the pinned tree** (grep is empty) |
| C++ mod load | `UE4SS/src/Mod/CppMod.cpp:13-65` | `AddDllDirectory(<mod>/dlls)` + `LoadLibraryExW(main.dll, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR \| LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` |
| OBSE64 plugin scan | `ue4ss/mod/OBSE64Interop/src/dllmain.cpp:165-236, 261` | `GetModuleFileNameW(nullptr)` → exe dir → `OBSE/Plugins`, `directory_iterator`, `is_regular_file` |
| OBSE64 plugin load | `ue4ss/mod/OBSE64Interop/src/OBSE64PluginManager.cpp:849, 922-933` | `std::filesystem::absolute` (not `canonical`), `LoadLibraryExW(... SEARCH_DLL_LOAD_DIR \| DEFAULT_DIRS)`, failures logged with `GetLastError` |
| Runtime identity | `dllmain.cpp:29-31, 262-269` | in-memory call-anchor check at fixed RVAs; not filesystem based; degrades to "unverified" |

Only one API in this chain is known to look through USVFS:
`std::filesystem::weakly_canonical` (MSVC uses `GetFinalPathNameByHandleW`),
used by UE4SS in `make_compatible_path` (`UE4SSProgram.cpp:2098-2108`) and for
`UE4SS_MODS_PATHS` (:239). Neither is on the default path; UNBSE ships no
`ModsFolderPath`, `+ModsFolderPaths`, or `ControllingModsTxt` override.

## 3. Mod Organizer 2

Source of truth: `modorganizer-basic_games/games/game_oblivion_remaster.py`
and `games/oblivion_remaster/*` (master, fetched 2026-08-30; plugin version
`0.1.0-b.1`, author Silarn). The 2.5.3 betas that carry this plugin are
distributed through the MO2 Discord, not GitHub releases, so this study is
based on the plugin source rather than a live run (a 2.5.2 build refused to
load the master plugin).

### 3.1 Why a virtual `dwmapi.dll` never loads

MO2 starts hooked executables through `usvfsCreateProcessHooked()`
(`usvfs/src/usvfs_dll/usvfs.cpp`): the process is created suspended,
`injectProcess()` (`usvfs_helper/inject.cpp`, `tinjectlib/injectlib.cpp`
`InjectDLLEIP`) writes a stub into the target and points the main thread's
`Rip` at it, and the stub runs `LoadLibraryW(usvfs_x64.dll)` + `InitHooks`
before jumping back to the original entry. That stub executes at the thread's
user-mode start, i.e. *after* `ntdll` has resolved the executable's static
imports — the loader has already looked for `dwmapi.dll` in the real `Win64`
and then System32. A `dwmapi.dll` that only exists in an MO2 mod folder is
never seen. MO2's maintainers describe this as a known limitation
(modorganizer issue #1164, BepInEx `winhttp.dll`: "Usvfs ... loads after all
dlls are loaded ... there is nothing we know that can be done"); the usvfs
README says it "may not be active at the time dependent dlls are loaded". This
is the same reason ENB/ReShade proxies must live in the real game folder. It
also means `UE4SS.log` and crash dumps written into a *virtual* `ue4ss/` end up
in MO2's Overwrite folder, which users mistake for "no log".

MO2's plugin encodes this (`activeProblems` / `fullDescription`):

> The UE4SS loader DLL is present (dwmapi.dll). This will not function
> out-of-the box with MO2's virtual filesystem. In order to resolve this,
> either delete the DLL and use the OBSE UE4SS Loader plugin, or rename the DLL
> (ex. 'ue4ss_loader.dll') and set it to force load with the game exe. Do this
> for any executable which runs the game, such as the OBSE64 loader.

The guided fix renames the **real** `Win64\dwmapi.dll` to `ue4ss_loader.dll`.
So the documented MO2 setup keeps the UE4SS runtime physically in the game
folder; only UE4SS *mods* are virtual.

Force-load mechanics (`usvfs/src/usvfs_dll/usvfs.cpp`, `InitHooks`): after
hooks are installed, for each `(process name, library)` pair registered by MO2
for the executable, `LoadLibraryExW(library, NULL, 0)` is called if
`std::filesystem::exists(library)`. Because `GetModuleFileNameW` is hooked with
an inverse (real→virtual) reroute and the proxy resolves `ue4ss/UE4SS.dll`
relative to its own module path, the rename + force-load route works when the
runtime is in the real folder (proxy name is irrelevant, see section 2). A
force-loaded proxy living in a *virtual* location is a corner case this study
did not run; note that the nested `LoadLibrary("<virtual>/ue4ss/UE4SS.dll")`
happens while the `LOAD_LIBRARY` hook group is already active on the thread
(`hookcallcontext.cpp`: a hook is inactive if its own group bit is set), so it
depends on the NT-level `NtOpenFile` reroute alone. Keep the runtime real.

### 3.2 The actual blocker: MO2 owns `mods.txt` and deletes `enabled.txt`

`game_oblivion_remaster.py`:

- `getModMappings()` maps mod-folder subdirectories: `Data` →
  `Content/Dev/ObvData/Data`, `Paks` → `Content/Paks`, `OBSE` → `Win64/OBSE`,
  `UE4SS` → **`Win64/ue4ss/Mods`**, `Movies`, `GameSettings`. There is no
  mapping for `Win64` itself; that is what Root Builder's `Root/` is for.
- `mappings()` maps the profile's `mods.txt` and `mods.json` **onto**
  `Win64/ue4ss/Mods/mods.txt` and `mods.json`, shadowing whatever is in the
  real folder.
- `initializeProfile()` seeds those files with `BPML_GenericFunctions` and
  `BPModLoaderMod` (`constants.py: DEFAULT_UE4SS_MODS`).

`oblivion_remaster/ue4ss/widget.py` (`UE4SSTabWidget`):

- `_parse_mod_files()` runs at UI init, on profile change, on mod install,
  removal and state change. For every active MO2 mod it looks at `UE4SS/` (or
  `Root/OblivionRemastered/Binaries/Win64/ue4ss/Mods`), adds an entry **only
  if `scripts/main.lua` exists**, and **unlinks `enabled.txt`** wherever it
  finds one. It then walks the *real* `game.ue4ssDirectory()` and does the
  same: list Lua mods, `Path(dir, "enabled.txt").unlink()`.
- `write_mod_list()` rewrites the profile `mods.txt` (`Name : 1|0`) and
  `mods.json` from that list whenever the model changes.
- `update_mod_files()` (on install / state change) unlinks `enabled.txt` again.

Consequence for UNBSE, regardless of how UE4SS itself is loaded:

1. `UNBSE` and `UNBSEOBSE64Interop` contain `dlls/main.dll` but no
   `scripts/main.lua` → never listed → never written to `mods.txt`.
2. Their `enabled.txt` markers are deleted (also from a manual install in the
   real game folder — the plugin edits the game directory directly).
3. UE4SS Part 1 sees a `mods.txt` without them, Part 2 finds no `enabled.txt`.
   Both mods stay stopped. `UE4SS.log` will show `Loading mods from: ...` and
   `Starting mods (from mods.txt ...)` without any `[UNBSE.Core]` line.

Manually adding `UNBSE : 1` to the profile `mods.txt` is undone the next time
the tab rewrites the file. This exactly matches "no solution can be found in
that approach".

### 3.3 What MO2's installer does with the archives

`oblivion_remaster/mod_data_checker.py`:

- `dataLooksValid()`: an archive containing `ue4ss/UE4SS.dll` **or**
  `OblivionRemastered/Binaries/Win64/ue4ss/UE4SS.dll` is FIXABLE.
- `fix()`: if `UE4SS.dll` is found, *every sibling of the `ue4ss` folder* is
  moved under `Root/OblivionRemastered/Binaries/Win64/` (MERGE). Both the
  current drop-in zip and the uncommitted `-game-root.zip` end up as
  `Root/OblivionRemastered/Binaries/Win64/{dwmapi.dll, ue4ss/...}`.
- `Root/` is only honoured with Kezyma's Root Builder. Its modes
  (`docs/rootbuilder.md`): Copy (default), Link (hard links), USVFS, USVFS +
  Link. The docs state USVFS mode "has compatibility issues with files that
  must be present at launch (exe, dll)". So with Root Builder in Copy/Link
  mode the runtime becomes real and UE4SS loads; in USVFS mode it does not.
  Either way section 3.2 still stops UNBSE's mods. The MO2 wiki also warns
  that Root Builder needs extra exclusions for this game or it "will attempt
  to cache many gigabytes of game data and hang MO2".
- A mods-only archive (`ue4ss/Mods/UNBSE/dlls/main.dll`, no Lua) is INVALID:
  the checker only accepts `Mods/<x>/scripts/main.lua` or the names `shared`,
  `npcappearancemanager`, `naturalbodymorph`. The user has to set the data
  directory by hand, and the mod is still not listed in the UE4SS tab.

`oblivion_remaster/script_extender.py` registers `obse64_loader.exe` as the
game's script extender (`OBSE/Plugins` as plugin path), and `executables()`
auto-adds an "OBSE64" launcher when the file exists — the OBSE64-centric
workflow UNBSE's install instructions ask users to dismantle.

### 3.4 The "OBSE64 UE4SS Loader" route

Nexus mod 3421 ("OBSE64 ue4ss Loader", xEpicBradx, v1.6.0 for runtime
1.512.105) is an OBSE64 plugin whose only job is to `LoadLibrary` UE4SS from
inside an OBSE64-launched game, so MO2 users can hide `dwmapi.dll` and launch
through `obse64_loader.exe`. OBSE64's loader does `CreateProcess(...
CREATE_SUSPENDED)` + remote `LoadLibraryA` of `obse64_1_512_105.dll`; USVFS
hooks `CreateProcessInternalW`, injects the child, and is therefore active by
the time the OBSE64 runtime loads `OBSE/Plugins`. The MO2 developer's sticky
on that page: "USVFS is unable to work properly when using dwmapi.dll to load
UE4SS ... OBSE allows it to hook the application before plugins are loaded".
It is "Method 1 – recommended" on the MO2 wiki page. For UNBSE this is a dead
end twice over: UNBSE's interop replaces OBSE64 (the README asks to delete
`obse64_loader.exe`, `obse64_steam_loader.dll`, `obse64_*.dll`), and if OBSE64
is left in place both runtimes load `OBSE/Plugins`.

The same reporter later added on the Nexus page (mod 5620) that "this MO2
issue appears on vortex, as well"; two other posts there ("game simply does
not launch with this ue4ss.dll", tried Vortex and manual; MagicLoader 2 runs
but the game does not boot) are not manager-specific and are outside this
study's scope, but section 4.4 items 1 and 3 are plausible causes for the
Vortex variants.

## 4. Vortex

Source of truth: the installed extension
`%APPDATA%\Vortex\plugins\...Oblivion Remastered Vortex Support v0.1.8\index.js`
(same code as `Nexus-Mods/game-oblivionremastered` main), Vortex 1.13.7 on this
machine, plus Vortex core for the deploy blacklist.

### 4.1 No VFS, so the loader is fine

`registerGame` sets `supportsSymlinks: false`; deployment is hard links (or
move) into the real game folder. A hard-linked `dwmapi.dll` next to the exe is
a real file: the proxy and UE4SS load exactly as in a manual install. Launch
goes through Steam (`requiresLauncher` → appId 2623190) or the primary tool.

### 4.2 How the archive is classified

Installers by priority (lower wins): `oblivionremastered-ue4ss` (10),
`-root-mod` (15), `-lua-installer` (30). `testUE4SSInjector` is simply "any
file named `UE4SS-settings.ini`" — so the UNBSE archive **is the UE4SS mod** as
far as Vortex is concerned. `installUE4SSInjector` re-roots everything at the
depth of `dwmapi.dll` under `OblivionRemastered/Binaries/Win64/` and sets the
default mod type (game root). Effects:

- Layout is preserved correctly (`dwmapi.dll`, `ue4ss/UE4SS.dll`,
  `ue4ss/Mods/UNBSE/...`). Files without an extension are dropped
  (`path.extname(...) !== ''`): `ue4ss/LICENSE` is silently not installed.
- If a variant ever ships `UE4SS-settings.ini` **without** `dwmapi.dll`,
  `cutoffIdx` is -1 and every file is flattened into `Win64\<basename>`.
- It generates `mods.json.original` (default `BPML_GenericFunctions`,
  `BPModLoaderMod`) because the archive has no `mods.json`.
- A mods-only archive without Lua is caught by the "Binaries Folder" mod type
  (any `.dll`) and deployed to `Win64` preserving relative paths — placement is
  right, but such a mod is never a "LUA Mod", so Vortex never touches
  `mods.json` for it.

### 4.3 Enablement under Vortex

- Vortex manages **only `mods.json`** (`MODS_FILE = 'mods.json'`;
  `modsFile.ts: onAddMod/onRemoveMod` add or remove one entry by `folderId`,
  for mods of type `oblivionremastered-lua-modtype` only). It never writes
  `mods.txt`. The pinned UE4SS ignores `mods.json`, so this bookkeeping is a
  no-op for UNBSE either way.
- `details.ignoreDeploy = ['ALTAR.INI','MODS.JSON','MODS.JSON.ORIGINAL',
  'ENABLED.TXT']`. Vortex core's `BlacklistSet.has()` runs
  `minimatch(value, pat, { nocase: true })` against the file path, with no
  `matchBase` and no `**/` prefix, so `ENABLED.TXT` matches only a root-level
  file. Nested `ue4ss/Mods/UNBSE/enabled.txt` **is deployed** (code reading;
  the reporter's Vortex test should confirm the file exists in the game folder
  after deploy). The `enableBPModLoader` code path writing a nested
  `enabled.txt` into the UE4SS mod and redeploying it is consistent with this.

So under Vortex the two UNBSE mods should start. The breakers are elsewhere:

### 4.4 The actual Vortex breakers

1. **OBSE64 requirement and primary tool.** `EXTENSION_REQUIREMENTS.steam`
   includes `obseRequirement` (Nexus mod 282, deployed to `Win64` as a
   Binaries mod) and `trySetPrimaryTool` makes `obse64_loader.exe` the primary
   launch tool. A Vortex user therefore has OBSE64 installed and launches
   through it unless they opt out. With UNBSE also present, OBSE64's runtime
   and UNBSE's interop both scan and load `OBSE/Plugins` → double-loaded
   plugins, double `PostLoad`, crashes. (The OBSE64-UE4SS-Loader author reports
   exactly "huge crash issues trying to get UE4SS and OBSE64 to work
   simultaneously while running my mods through Vortex".)
2. **"UE4SS missing or corrupted" nag.** `testBluePrintModManager` runs on
   `gamemode-activated` and `did-deploy`, finds the mod that contains
   `UE4SS-settings.ini` (UNBSE), and checks for
   `<staging>/<mod>/OblivionRemastered/Binaries/Win64/ue4ss/Mods/BPModLoaderMod`.
   UNBSE ships no `BPModLoaderMod`, so `reinstallUE4SS` raises a
   non-dismissable, non-suppressible warning after every deploy, whose "Fix"
   tries to download Nexus UE4SS (mod 32).
3. **Conflict with Nexus "UE4SS for OblivionRemastered" (mod 32).** Anyone who
   installed a Lua/Blueprint mod before UNBSE was prompted to install it. Its
   `dwmapi.dll`, `ue4ss/UE4SS.dll`, `ue4ss/UE4SS-settings.ini` collide with
   UNBSE's; those names are not in `IGNORE_CONFLICTS`. Vortex demands a rule,
   refuses to launch the game while conflicts are unresolved
   (`unsolvedConflictsCheck.ts`), and if mod 32 wins, UNBSE's `main.dll` runs
   against a foreign `UE4SS.dll` (v3.0.1-394 or -447) and settings file. The
   mod-32 author's sticky says "Recommended installation is manual, NOT VIA
   VORTEX"; the extension changelog (0.1.4) confirms Vortex pulls UE4SS from
   mod 32.
4. **Purge/uninstall leaves UE4SS runtime files** (`UE4SS.log`, crash dumps,
   `imgui.ini`) behind; not a UNBSE bug, but users will report "it did not
   uninstall".

## 5. Reproduction notes

- Game: `D:\SteamLibrary\steamapps\common\Oblivion Remastered`, UNBSE
  `0.11.0-rc.1` manually installed. `UE4SS.log` (2026-08-30 05:23) shows the
  healthy manual path: `Starting mods (from enabled.txt ...)`, `Mod 'UNBSE'
  has enabled.txt, starting mod.`, `[UNBSE.Core] {... "status":"constructed"}`,
  interop loading three `OBSE/Plugins` DLLs. This is the log signature the
  reporter's MO2 log will be missing.
- MO2: `C:\Modding\MO2` (2.5.3 per the leftover instance ini) is no longer on
  disk; the AppData instance still points at the old `C:` game path. The 2.5.3
  betas are Discord-only; the GitHub CI artifact is a 2.5.2-versioned build
  without `plugins/`. A portable 2.5.2 + basic_games master refused to open
  the instance ("game plugin doesn't exist") — the plugin needs the 2.5.3
  Python API — so section 3 is source-derived, not observed. Test-run backups
  of `enabled.txt` in the game folder were restored (7/7).
- Vortex 1.13.7 with the OBR extension 0.1.8 is installed
  (`%APPDATA%\Vortex\oblivionremastered`, empty staging). Section 4 is derived
  from the installed extension code and Vortex core; no deploy was run.

## 6. What the reporter can check (MO2 2.5.3b12)

1. After renaming `dwmapi.dll` → `ue4ss_loader.dll` in the real `Win64` and
   force-loading it for `OblivionRemastered-Win64-Shipping.exe`: open
   `Win64\ue4ss\UE4SS.log`. If UE4SS started, the log exists and shows
   `Loading mods from: ...\ue4ss\Mods`. Expect no `[UNBSE.Core]` line.
2. Check `Win64\ue4ss\Mods\UNBSE\enabled.txt` and
   `Win64\ue4ss\Mods\UNBSEOBSE64Interop\enabled.txt` — they will be gone (MO2's
   UE4SS tab removed them).
3. Open MO2's UE4SS Mods tab: `UNBSE` is not listed (no `Scripts\main.lua`).
4. Temporary proof (until a release fix): create an empty
   `Win64\ue4ss\Mods\UNBSE\Scripts\main.lua` and
   `Win64\ue4ss\Mods\UNBSEOBSE64Interop\Scripts\main.lua`, restart MO2, tick
   both in the UE4SS Mods tab, launch. `UE4SS.log` should now show
   `Starting C++ mod 'UNBSE'` and the `[UNBSE.Core]` / `[UNBSE.OBSE64Interop]`
   lines. UE4SS starts both mod types by the same name from `mods.txt`
   (`start_mods<LuaMod>` and `start_mods<CppMod>`, `UE4SSProgram.cpp:1537-1547`).

## 7. Proposed changes

Ordered by cost. Items 1–3 are packaging only and fix the reported MO2 case
and the Vortex nag; 4–5 harden the runtime; 6 is the MO2-native loader.

1. **Ship a Lua stub in each UNBSE mod folder** —
   `ue4ss/Mods/UNBSE/Scripts/main.lua` and
   `ue4ss/Mods/UNBSEOBSE64Interop/Scripts/main.lua` (a comment-only file, or a
   one-line `print`). Effects: MO2 lists both mods, writes them to
   `mods.txt`/`mods.json`, and a mods-only archive becomes FIXABLE (moved to
   `UE4SS/<name>` → `Win64/ue4ss/Mods/<name>`); Vortex classifies a mods-only
   archive as a LUA Mod, deploys it to `ue4ss/Mods/<name>`, and adds a
   `mods.json` entry. UE4SS will also instantiate an (idle) `LuaMod` for each;
   `setup_mods` handles a folder with both `scripts/` and `dlls/`. Keep the
   `enabled.txt` markers for manual installs.
2. **Split the release into two archives plus the existing drop-in:**
   - `UNBSE-<ver>-runtime.zip`: `dwmapi.dll`, `ue4ss/UE4SS.dll`,
     `ue4ss/UE4SS-settings.ini`, `ue4ss/LICENSE.txt` (give it an extension so
     Vortex keeps it), `ue4ss/Mods/BPModLoaderMod` and
     `ue4ss/Mods/BPML_GenericFunctions` from the pinned UE4SS assets (silences
     Vortex's "missing or corrupted" check and matches what every other UE4SS
     install has), and an empty-array `ue4ss/Mods/mods.json`. Document: install
     into the real game folder on every manager; for MO2 rename the proxy and
     force-load it (or use item 6).
   - `UNBSE-<ver>-mods.zip`: `ue4ss/Mods/UNBSE/...` and
     `ue4ss/Mods/UNBSEOBSE64Interop/...` with the Lua stubs. This is the
     manager-installable part.
   - Keep `UNBSE-<ver>.zip` (drop-in) for manual installs; the
     `-game-root.zip` adds nothing for managers (MO2 reroutes it to `Root/`
     exactly like the flat one, Vortex re-roots at `dwmapi.dll` anyway).
3. **Document manager installs** in `README.md` and the Nexus page: MO2
   (runtime real + rename/force-load, mods via MO2, Root Builder optional and
   only in Copy/Link mode), Vortex (disable/uninstall the OBSE64 requirement
   mod, set the primary tool back to Steam, resolve the conflict in favour of
   UNBSE if Nexus UE4SS is installed, or uninstall it), and the "remove
   OBSE64" prerequisite phrased for manager users.
4. **Interop stand-down when a legacy OBSE64 runtime is present.** In
   `OBSE64Interop/src/dllmain.cpp`, before scanning `OBSE/Plugins`, check
   `GetModuleHandleW(L"obse64_1_512_105.dll")` (any `obse64_*.dll` /
   `obse64_steam_loader.dll`) and, if found, log a structured
   `runtime-conflict` event and skip plugin loading. Prevents the double-load
   crash when a manager launches through `obse64_loader.exe`.
5. **Foundation patch: read `mods.json`** (mirror the shape both managers
   write: `[{"mod_name","mod_enabled"}]`) as a third enable source in
   `start_mods`, and optionally treat `unbse-mod-manifest.json` as an enable
   marker. This makes the managers' own bookkeeping effective against the
   pinned runtime and removes the dependency on `enabled.txt` surviving. Small,
   self-contained change to `UE4SSProgram.cpp` alongside the existing patches.
6. **MO2-native loader (`UNBSELoader.exe`).** The retained `src/Loader.cpp`
   already implements the SKSE/OBSE64 pattern (`CreateProcessW` suspended,
   `WriteProcessMemory` bootstrap thunk, `CreateRemoteThread` → `LoadLibraryW`).
   Re-pointed at `ue4ss\UE4SS.dll` (bypassing `dwmapi.dll`) it gives MO2 users
   the familiar "add the loader as an executable" workflow: USVFS follows the
   child, UE4SS is loaded by its real path, `GetModuleFileNameW` yields the
   real `Win64\ue4ss` (the virtual root), and `Mods` is enumerated through the
   VFS. To verify before shipping: ordering between USVFS's injection in the
   `CreateProcessInternalW` hook and our remote thread; UE4SS initialising
   from a remote thread rather than the import-time `DllMain`; Steam
   relaunch behaviour (`steam_appid.txt` is already next to the exe).

Not recommended: shipping a `ue4ss/Mods/mods.txt` in the drop-in (it would
overwrite a manual user's existing load order), or relying on MO2's
`ue4ss_use_root_builder` setting (opt-in, and it still deletes `enabled.txt`).

## 8. Evidence index

UNBSE (this repository):
- `ue4ss/mod/OBSE64Interop/src/dllmain.cpp:165-173` (`GetModuleFileNameW(nullptr)`), `:201-236` (`OBSE/Plugins` scan), `:261` (`GameRoot = Executable.parent_path()`), `:29-31, 262-286` (RVA anchor check, graceful degradation).
- `ue4ss/mod/OBSE64Interop/src/OBSE64PluginManager.cpp:849` (`std::filesystem::absolute`), `:922-933` (`LoadLibraryExW` flags, error logging).
- `ue4ss/mod/UNBSE/src/AddonRegistry.cpp:710-724` (executable name for runtime info only).
- `ue4ss/scripts/Package-UNBSERelease.ps1:105-141` (archive assembly; no `mods.txt`), `ue4ss/scripts/Set-UNBSECoreManifestBoundary.ps1:34` (build-time reparse check only).
- `ue4ss/foundation-manifest.json` (`requiredRuntimeArtifacts`: `dwmapi.dll`, `ue4ss/UE4SS.dll`, `ue4ss/UE4SS-settings.ini`, `ue4ss/LICENSE`).

UE4SS (pinned `68dd45cb`, `out/ue4ss-source`):
- `UE4SS/proxy_generator/main.cpp:239-252` (`load_original_dll`), `:309-372` (`load_ue4ss_dll`), `:380-395` (`DllMain`).
- `UE4SS/src/UE4SSProgram.cpp:192-350` (constructor: `setup_paths`, `setup_mod_directory_path`, `setup_mods` run in `DllMain`), `:454-539`, `:1268-1316`, `:1446-1611`, `:2098-2108`.
- `UE4SS/src/Mod/CppMod.cpp:13-65`.

Mod Organizer 2:
- `modorganizer-basic_games/games/game_oblivion_remaster.py` (`GameDataPath`, `getModMappings`, `mappings`, `initializeProfile`, `write_default_mods`, `activeProblems`, `fullDescription`, `startGuidedFix`, setting `ue4ss_use_root_builder`).
- `games/oblivion_remaster/ue4ss/widget.py` (`_parse_mod_files`, `update_mod_files`, `write_mod_list`), `ue4ss/model.py`, `constants.py`, `mod_data_checker.py` (`dataLooksValid`, `fix`), `script_extender.py`.
- Wiki: https://github.com/ModOrganizer2/modorganizer-basic_games/wiki/Game:-Elder-Scrolls-IV:-Oblivion-Remastered
- USVFS: `src/usvfs_dll/hookmanager.cpp` (hooked API list), `hooks/kernel32.cpp` (`hook_LoadLibraryExW`, `hook_GetModuleFileNameW` with inverse reroute, `CreateProcessInternalW` child injection), `usvfs.cpp` (`usvfsCreateProcessHooked`, `usvfsForceLoadLibrary`, `InitHooks` force-load loop), `usvfs_helper/inject.cpp` + `tinjectlib/injectlib.cpp` (`InjectDLLEIP`), `hookcallcontext.cpp` (hook group exclusion rule).
- MO2 core: https://github.com/ModOrganizer2/modorganizer/pull/2241 (Oblivion Remastered meta PR, `getModMappings`), https://github.com/ModOrganizer2/modorganizer/issues/1164 (proxy DLLs load before USVFS — "nothing we know that can be done"), wiki "Executables window" (force load libraries "mostly for OBSE").
- Root Builder: https://github.com/Kezyma/ModOrganizer-Plugins/blob/main/docs/rootbuilder.md
- Nexus: OBSE64 ue4ss Loader (mod 3421, sticky by the MO2 plugin author), UNBSE page (mod 5620) posts of 2026-08-30.

Vortex:
- Installed extension `index.js` 0.1.8: constants (`MODS_FILE`, `UE4SS_ENABLED_FILE`, `IGNORE_DEPLOY`, `IGNORE_CONFLICTS`, `UE4SSRequirement` modId 32, `obseRequirement` modId 282, `EXTENSION_REQUIREMENTS`), `registerInstaller`/`registerModType` priorities, `testUE4SSInjector`/`installUE4SSInjector`, `testLuaMod`/`installLuaMod`, `testRootMod`, `modTypes` (`getBinariesPath`, `testBinariesPath`, `getLUAPath`), `testBluePrintModManager`/`reinstallUE4SS`/`enableBPModLoader`, `ensureModsFile`, `isLuaMod`, `resolveUE4SSPath`, `trySetPrimaryTool`.
- Repository: https://github.com/Nexus-Mods/game-oblivionremastered ; Vortex core `src/renderer/src/extensions/mod_management/util/BlacklistSet.ts`, `extensions/mod-dependency-manager/src/unsolvedConflictsCheck.ts`.
- Nexus: UE4SS for OblivionRemastered (mod 32) sticky "Recommended installation is manual, NOT VIA VORTEX"; OBSE64 (mod 282); OBSE64 ue4ss Loader (mod 3421); Vortex extension page (site mod 1270) posts.
