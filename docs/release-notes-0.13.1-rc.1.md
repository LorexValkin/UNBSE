# UNBSE 0.13.1-rc.1

UNBSE 0.13.1-rc.1 is a focused release candidate for the Steam
`OblivionRemastered-Win64-Shipping.exe` runtime `1.512.105.0`.

## Change

- The packaged and repaired `UE4SS-settings.ini` now contains
  `[UNBSE] EnablePluginVersionWarning = 0`. With the default `0`, normal
  `UNBSELoader.exe` launches do not scan `OBSE/Plugins` for declared game
  versions, print an `Invalid Version Mod` list, show the plugin-version warning
  dialog, block launch for that warning, or persist plugin-warning acceptance
  state.
- Users can set `EnablePluginVersionWarning = 1` to restore the warning, launch
  gate, and exact-binary acceptance behavior. Other values are repaired to the
  off default.
- `UNBSELoader.exe --validate-only` retains the static plugin declaration
  report as an explicit, non-interactive diagnostic regardless of the toggle.
  It does not load plugin DLLs, show a popup, or launch the game.
- The separate asset-container preflight and warning remain unchanged.

This change removes a warning gate; it does not make every native plugin
compatible. Native plugin support remains plugin-by-plugin, and this release
does not claim support for game builds other than Steam `1.512.105.0`.

## Installation

Install `UNBSE-0.13.1-rc.1.zip` as the same single manual, Vortex, or Mod
Organizer 2 package described in the README. Remove the legacy OBSE64 loader
and runtime files first, preserve `OBSE/Plugins`, and remove a separate UE4SS
package before installing UNBSE.

The public package contains the pinned UE4SS runtime, UNBSE core and OBSE64
interoperability modules, native loader, SDK headers, documentation, manifest,
and checksums. UNBSE-produced and patched PE binaries are Authenticode-signed
and timestamped.
