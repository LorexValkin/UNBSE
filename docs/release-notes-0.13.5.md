# UNBSE 0.13.5 signed production candidate

UNBSE 0.13.5 is a signed production compatibility candidate for
`OblivionRemastered-Win64-Shipping.exe` version `1.512.105.0`. It is not
validated or release-ready. Local compilation, packaging, checksum, and
structural test results are engineering evidence only; real in-game testing is
mandatory before the status can change.

Every produced or patched PE in the package is Authenticode-signed by Computer
Works and timestamped: `UNBSELoader.exe`, the patched `UE4SS.dll`, the UNBSE
core module, and the OBSE64 interoperability module.

## Compatibility change

- The bundled UE4SS host now classifies supported C++ mod interfaces by their
  validated virtual-table shape instead of a mod name.
- Current 16-slot C++ mods retain the normal callback path.
- Legacy 11-slot C++ mods use an adapter for their historical vector-based Lua
  start/stop callbacks and DLL-load callback. The newer
  `on_cpp_mods_loaded` callback is not invoked outside that legacy interface.
- Successful legacy adapters are reported once per bulk startup as
  `Legacy C++ mods adapted (11-slot -> current callbacks): [names]`; routine
  callback dispatch is silent, while an individual rejection or startup failure
  retains the affected mod name and reason.
- Unsupported or unreadable interfaces are rejected before shifted virtual
  callbacks can be invoked.
- The ConsoleUtils 1.1 EngineTick compatibility path remains an exact,
  independent rule. It is not applied to other legacy mods.

The compatibility investigation used an unmodified third-party legacy mod as
evidence for the ABI shape. No third-party binary is included in UNBSE or its
source archive, and compatibility remains plugin-by-plugin.

## Current in-game evidence

A human Steam-runtime launch on 2026-08-31 confirmed that two unmodified legacy
11-slot mods, ConsoleUtils 1.1 and MultiEnchant, were adapted in the same
startup while the current 16-slot OBRPlayableRaces mod initialized alongside
them. Both legacy mods returned a successful start result, MultiEnchant applied
its three native patches, the ConsoleUtils game-thread compatibility probe
completed, OBRPlayableRaces registered with UNBSE, and lifecycle processing
continued through DataLoaded without an ABI rejection or crash in the retained
log.

This is real runtime evidence for simultaneous legacy/current initialization;
it does not replace feature-level exercise, a clean shutdown, or a relaunch
test.

## Required in-game validation

Do not mark this candidate validated or release-ready until a human tester has
completed and retained evidence for all of the following on the supported Steam
runtime:

1. Launch from a clean UNBSE install and confirm the title reaches playable
   state without a new crash or startup error.
2. Load at least one current 16-slot C++ mod and confirm its startup, Lua
   callbacks, post-C++-mods-loaded callback, and core behavior.
3. Load at least two unmodified legacy 11-slot C++ mods together, confirm both
   names appear in the one-line legacy adapter summary in `UE4SS.log`, reach
   playable state, exercise both mods' visible behavior, and confirm no crash
   at the post-load boundary.
4. Test ConsoleUtils 1.1 separately and confirm its EngineTick compatibility
   probe completes once without a duplicate hook or regression.
5. Save or load as appropriate, play long enough to exercise recurring update
   callbacks, exit cleanly, then relaunch once.
6. Review the complete logs and Windows crash output for new faults, and record
   the tested mod versions, game executable hash/version, configuration, and
   observations.

Until every item passes in the actual game, 0.13.5 remains a signed production
candidate rather than a validated release.
