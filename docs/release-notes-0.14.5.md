# UNBSE 0.14.5 — final planned release for now

UNBSE 0.14.5 targets only the Steam
`OblivionRemastered-Win64-Shipping.exe` runtime version `1.512.105.0`.
It is a focused ScriptService capacity and console-stability update to 0.14.1
and does not broaden runtime or native-plugin compatibility claims.

## Maintenance note

0.14.5 is the last planned UNBSE release for the foreseeable future. After this
release, the project will no longer investigate or implement compatibility
fixes for individual third-party mods. Future work, if any, will be limited to
UNBSE's own loader/runtime, its documented public interfaces, and supported
game-runtime changes. Mod-specific bugs and compatibility changes belong with
the authors of those mods.

## Changes since 0.14.1

- Raised the ScriptService registry's fixed Lua VM capacity from 256 to 4096.
  The registry remains bounded and retains fixed storage, stable binding
  addresses, locking, detach/reuse behavior, shutdown semantics, public ABI,
  and existing result codes.
- Added a regression matching the observed 42-mod Lua load order. It confirms
  that NaturalBodyMorph and every later mod receive
  `capability:true,resultCode:0` while the existing 64-VM, duplicate,
  detach/reuse, thread-mismatch, and configured-boundary coverage remains in
  place.
- NaturalBodyMorph continued loading and applying morphs in the preserved
  pre-fix log after receiving result code 5. That result denotes only an UNBSE
  ScriptService attachment failure; this update does not modify, intercept, or
  package Natural Bodies files or data.
- Disabled Windows console QuickEdit in the UE4SS-owned terminal while
  preserving the remaining input-mode flags. This prevents an accidental text
  selection from blocking console writes and appearing to freeze the game until
  keyboard input dismisses the selection.
- Removed the upstream `dwmapi.dll` proxy from the install archive. The native
  launcher already injects the pinned UE4SS host after MO2's USVFS is available;
  omitting the early proxy prevents the MO2 startup race and clears its generic
  loader warning. The launcher and foundation audit now reject leftover default
  or commonly renamed proxy DLLs with removal guidance.
- Documented MO2's profile-local `mods.txt` and `mods.json` mapping. Those files
  remain in the selected profile and are exposed at `ue4ss/Mods` through USVFS;
  they do not need to be copied into the global game installation.
- Contained Lua action registry lookup failures inside UE4SS's existing
  per-action exception boundary. A broken callback from one mod can no longer
  unwind through the shared engine-tick detour and stop queued actions for every
  Lua mod. This hardens the shared runtime; it does not modify or redistribute
  third-party mod files.

## Installation and compatibility

Remove the legacy `obse64_loader.exe`, `obse64_steam_loader.dll`, and matching
`obse64_*.dll` runtime before installing UNBSE. Preserve `OBSE/Plugins`, extract
the single `UNBSE-0.14.5.zip` archive into the game's `Win64` directory, and
launch the game normally.

An OBSE64 plugin that requires Address Library still needs the matching
`versionlib-1-512-105-0.bin` table to function. Without it, UNBSE rejects the
affected plugin structurally instead of executing relocation-dependent code.

Native plugin compatibility remains plugin-by-plugin. The unchanged 0.14.1
compatibility review is republished under the 0.14.5 artifact name for release
consistency; it is not evidence of new plugin validation.

## Verification status

The packaged UE4SS host, UNBSE core, OBSE64 interoperability module, and launcher
are Authenticode signed by Computer Works and timestamped. Targeted
ScriptService registry, observed-load-order, source-pin, production-build,
package-structure, checksum, archive-content, and Authenticode checks pass
locally. Human in-game testing has not been completed, so 0.14.5 is published as
a prerelease and is not claimed as validated.
