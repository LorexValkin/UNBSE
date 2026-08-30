# UNBSE 0.12.0

UNBSE 0.12.0 is the first stable mod-manager release. It targets only the
Steam `OblivionRemastered-Win64-Shipping.exe` runtime `1.512.105.0`.

## Highlights

- One `UNBSE-0.12.0.zip` now serves manual drag-and-drop, Vortex, and Mod
  Organizer 2 installations. The archive contains the pinned patched UE4SS
  host, both UNBSE modules, the native prelaunch loader, SDK headers, install
  guidance, a package manifest, and internal checksums.
- The patched host now starts the bundled `UNBSE` and
  `UNBSEOBSE64Interop` foundation modules independently of manager-controlled
  `enabled.txt` and `mods.txt` state. This fixes the observed Vortex failure in
  which the manager deployed both DLLs but intentionally withheld their
  zero-byte `enabled.txt` files.
- `UNBSELoader.exe` now supports manager-aware launch. It verifies the exact
  packaged UE4SS host, locates the Steam game, and starts the game only after
  MO2's virtual filesystem is active when launched through MO2.
- The UE4SS console remains visible. Each C++ lifecycle callback is logged with
  its module identity so a terminal startup line identifies the callback active
  at a failure boundary.

## Prelaunch compatibility checks

- Native DLL declarations in `OBSE/Plugins` are inspected without loading or
  executing the plugin. A grouped `Invalid Version Mod` warning reports plugins
  that do not explicitly support runtime `1.512.105.0`, claim version
  independence without naming it, have unreadable declarations, or lack the
  required load export.
- `.pak`, `.utoc`, and `.ucas` files visible under the game's Paks directory are
  checked before launch. The bounded checks cover IoStore magic and table
  bounds, container identity and UCAS partitions, the current retail TOC
  layout, inspectable container-header version/identity, and pak footer/index
  integrity.
- Warning dialogs default to **No**. Choosing **Yes** remembers the exact game
  and plugin/container evidence; changed binaries are reviewed again.
- `UNBSELoader.exe --validate-only` reports the same findings without showing a
  dialog or launching the game.

These are early-risk checks, not compatibility certification. Native support
remains plugin-by-plugin. The asset pass validates the container envelope and
an inspectable top-level serialization header; it does not deserialize every
cooked package, custom version, or asset schema.

## Compatibility and diagnostics fixes

- Zero-byte OBSE64 local or branch trampoline requests are now accepted as a
  successful no-allocation request, matching plugins that use zero size as a
  capability probe.
- The release distinguishes the pinned upstream banner from proof that UNBSE is
  active. A healthy log contains marker-independent bootstrap lines for both
  bundled modules, successful `UE4SS.CppModLifecycle` starts, and UNBSE
  foundation/interoperability records. Reaching `Event loop start` without
  those records means the modules did not start.

## Upgrade notes

Remove legacy `obse64_loader.exe`, `obse64_steam_loader.dll`, and the matching
`obse64_*.dll` runtime before installation. Preserve `OBSE/Plugins`.

For Vortex, remove the separate community UE4SS package (Nexus mod 32), disable
the legacy OBSE64 requirement if Vortex installed it, install
`UNBSE-0.12.0.zip`, and make `UNBSELoader.exe` the primary tool. For MO2,
install the same archive, accept the Oblivion Remastered plugin's automatic
file-tree fix, and launch the loader from the resulting physical `Root` tree.
