# UNBSE 0.13.2-rc.2

UNBSE 0.13.2-rc.2 is an unsigned rapid-test release candidate for the Steam
`OblivionRemastered-Win64-Shipping.exe` runtime `1.512.105.0`. It is not a
publishable signed build.

## Changes

- The generated `UE4SS-settings.ini` now places the populated `[UNBSE]`
  section before UE4SS's stock empty `[ExperimentalFeatures]` section. This
  prevents the UE4SS INI parser error previously reported at the first UNBSE
  key and preserves both warning toggles.
- The packaged debug GUI profile is enabled, visible at startup, rendered with
  DirectX 11 on its external thread, and retains UE4SS's stock Ctrl+O toggle.
- The packaged UObject array cache default remains `false` for startup safety.
  Developers can set `General.bUseUObjectArrayCache = true`; the loader
  preserves either valid lowercase value so cache-dependent Live View features
  can be enabled deliberately.
- `[UNBSE] EnablePluginVersionWarning = 0` and
  `[UNBSE] EnableAssetContainerWarning = 0` remain the normal-launch defaults.
  Set either value to `1` to opt in to its resizable, scrollable warning window.
- `UNBSELoader.exe --validate-only` still performs both checks without showing
  a popup or launching the game.

The repaired live profile received a human game-launch test before packaging:
the root proxy and patched UE4SS host loaded, UNBSE 0.13.2 started, OBSE64
interop initialized, and the debug GUI appeared. The rebuilt rc.2 archive is
still subject to its own archive and installation verification.

## Installation

Install `UNBSE-0.13.2-rc.2.zip` as one manual, Vortex, or Mod Organizer 2
package. Remove a separate UE4SS package and the legacy OBSE64 loader/runtime
files first, but preserve `OBSE/Plugins`.

This rapid-test package is intentionally unsigned to shorten the test cycle.
It is rebuilt from pinned source, packaged with reproducibility inputs, and
accompanied by SHA-256 checksums.
