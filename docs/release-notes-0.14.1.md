# UNBSE 0.14.1 signed update candidate

UNBSE 0.14.1 targets only the Steam
`OblivionRemastered-Win64-Shipping.exe` runtime version `1.512.105.0`.
It is a focused update to 0.14.0 and does not broaden runtime or native-plugin
compatibility claims.

## Changes since 0.14.0

- Raised the ScriptService registry's fixed Lua VM capacity from 16 to 256.
  The registry retains fixed storage, stable binding addresses, locking,
  detach/reuse behavior, shutdown semantics, public ABI, and result codes.
- Added regression coverage for 64 unique VM attachments, duplicate
  idempotency, detach/reuse, thread mismatches, and the configured capacity
  boundary.
- Changed a missing required Address Library table from an unverified load
  attempt into a structural blocker. UNBSE now rejects the affected plugin
  before executing its relocation-dependent code.

## Installation and compatibility

Remove the legacy `obse64_loader.exe`, `obse64_steam_loader.dll`, and matching
`obse64_*.dll` runtime before installing UNBSE. Preserve `OBSE/Plugins`, extract
the single `UNBSE-0.14.1.zip` archive into the game's `Win64` directory, and
launch the game normally.

An OBSE64 plugin that requires Address Library still needs the matching
`versionlib-1-512-105-0.bin` table to function. Without it, the expected 0.14.1
behavior is a logged structural rejection instead of executing the plugin.

Native plugin compatibility remains plugin-by-plugin. The unchanged 0.14.0
compatibility review is republished under the 0.14.1 artifact name for release
consistency; it is not evidence of new plugin validation.

## Verification status

The packaged UE4SS host, UNBSE core, OBSE64 interoperability module, and launcher
are Authenticode signed by Computer Works and timestamped. Local scanner,
plugin-manager, ScriptService registry, source-pin, production build,
package-structure, checksum, and Authenticode checks passed. Human in-game
testing is still required before calling 0.14.1 validated or release-ready.
