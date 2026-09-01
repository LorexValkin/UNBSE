# UNBSE 0.14.2 update candidate

UNBSE 0.14.2 targets only the Steam
`OblivionRemastered-Win64-Shipping.exe` runtime version `1.512.105.0`.
It is a focused ScriptService capacity update to 0.14.1 and does not broaden
runtime or native-plugin compatibility claims.

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

## Installation and compatibility

Remove the legacy `obse64_loader.exe`, `obse64_steam_loader.dll`, and matching
`obse64_*.dll` runtime before installing UNBSE. Preserve `OBSE/Plugins`, extract
the single `UNBSE-0.14.2.zip` archive into the game's `Win64` directory, and
launch the game normally.

An OBSE64 plugin that requires Address Library still needs the matching
`versionlib-1-512-105-0.bin` table to function. Without it, UNBSE rejects the
affected plugin structurally instead of executing relocation-dependent code.

Native plugin compatibility remains plugin-by-plugin. The unchanged 0.14.1
compatibility review is republished under the 0.14.2 artifact name for release
consistency; it is not evidence of new plugin validation.

## Verification status

Targeted ScriptService registry, observed-load-order, source-pin, foundation,
production-build, package-structure, checksum, Authenticode, and
archive-content checks pass locally. The packaged UE4SS host, UNBSE core,
OBSE64 interop, and launcher binaries carry timestamped Computer Works
signatures. Human in-game testing remains required before calling 0.14.2
validated or release-ready.
