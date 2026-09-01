# UNBSE 0.14.0 compatibility candidate

UNBSE 0.14.0 targets only the Steam
`OblivionRemastered-Win64-Shipping.exe` runtime `1.512.105.0`. The compatibility
status remains `in-game-testing-required`: static review, compilation,
packaging, checksums, and structural tests do not prove third-party mod behavior
inside the game.

The published artifacts carry timestamped Computer Works signatures on
`UNBSELoader.exe`, the patched `UE4SS.dll`, the UNBSE core module, and the
OBSE64 interoperability module. Local unsigned builds are engineering test
material and must not be redistributed as the public prerelease.

## Compatibility changes

- Legacy Lua mods using the historical three-argument form of
  `KismetSystemLibrary.ExecuteConsoleCommand` are adapted only when the pinned
  runtime reflects that exact function with four parameters. UNBSE supplies the
  omitted trailing value and retains strict arity checks everywhere else.
- The existing name-agnostic legacy 11-slot UE4SS C++ mod adapter remains in
  place beside current 16-slot dispatch. Unsupported or unreadable interfaces
  are rejected before shifted virtual callbacks can run.
- The exact ConsoleUtils 1.1 EngineTick rule remains independent from the
  generic legacy C++ adapter.
- Build and package audits now require the compiled Lua-adapter marker as well
  as the C++ lifecycle marker, preventing an incompletely patched UE4SS host
  from passing the release boundary.

## Reviewed UE4SS mod corpus

The 0.14.0 investigation statically reviewed 502 Nexus mods represented by 659
download packages and 8,683 inventoried files. The release includes
`UNBSE-0.14.0-mod-compatibility-review.csv` as a filterable sidecar with one row
per reviewed package, the evidence verdict, finding codes, adapter/dependency
coverage, and explicit runtime-proof boundary.

- 409 packages expose no surface applicable to the UNBSE host.
- 81 have no static host gap detected but still require in-game proof.
- 155 remain statically unresolved and require runtime evidence.
- 7 are covered by a UNBSE adapter and still require in-game proof.
- 6 have a mapped ConfigHelper or OBRConsole dependency provider; those
  standalone internal layers are not bundled into UNBSE.
- 1 package is confirmed incompatible: Nexus mod 3421's `ue4ss_Loader.dll`
  exports `OBSEPlugin_Preload` but lacks the required `OBSEPlugin_Load`. Do not
  install it with UNBSE.

The bounded Lua adapter covers 55 decoded calls across 20 mods and 21 packages.
The legacy C++ adapter covers 8 decoded DLL variants across 6 mods. The remaining
730 runtime-dependent occurrences are not declared compatible or incompatible;
they collapse into 231 unique read-only runtime probes.

For Lumen Remastered 2.6, the four observed `ExecuteConsoleCommand` errors are
covered by the bounded adapter. Lumen is not declared fully compatible because
its hook targets, reflected methods, timing, and recurring behavior still need
post-patch in-game evidence.

## Required in-game validation

Do not mark 0.14.0 validated or release-ready until a human tester has retained
evidence for all of the following on the supported Steam runtime:

1. Start from a clean UNBSE installation and reach playable state without a new
   crash or startup error.
2. Exercise at least one current 16-slot C++ mod through startup, Lua callbacks,
   post-C++-mods-loaded dispatch, and visible behavior.
3. Load at least two unmodified legacy 11-slot C++ mods together, verify both
   names in the one-line adapter summary, and exercise their visible behavior.
4. Test ConsoleUtils 1.1 separately and confirm its one-shot EngineTick probe
   completes without a duplicate hook or regression.
5. Run unmodified Lumen Remastered 2.6, observe the one-time Lua-adapter message,
   verify its console-variable changes, and confirm its recurring updates no
   longer emit UFunction arity errors.
6. Save or load as appropriate, play long enough to exercise recurring
   callbacks, exit cleanly, relaunch once, and review the complete logs and
   Windows crash output.

## Installation and removal

Remove legacy `obse64_loader.exe`, `obse64_steam_loader.dll`, and the matching
`obse64_*.dll` runtime before installing. Preserve `OBSE/Plugins`; UNBSE uses it
to discover compatible native plugins. Install the single
`UNBSE-0.14.0.zip` archive through Vortex, Mod Organizer 2, or manual extraction,
then launch through `UNBSELoader.exe`.

UNBSE does not bundle, rebuild, modify, or sign third-party mods. Native plugin
and UE4SS mod support remains plugin-by-plugin and is limited to the current
Steam runtime.
