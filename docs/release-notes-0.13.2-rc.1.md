# UNBSE 0.13.2-rc.1

UNBSE 0.13.2-rc.1 is an unsigned rapid-test release candidate for the Steam
`OblivionRemastered-Win64-Shipping.exe` runtime `1.512.105.0`. It is not a
publishable signed build.

## Changes

- The packaged and repaired `UE4SS-settings.ini` now contains
  `[UNBSE] EnableAssetContainerWarning = 0`. Normal launches skip the asset
  scan, warning, launch gate, and acceptance-state update by default. Set it to
  `1` to opt in.
- `[UNBSE] EnablePluginVersionWarning = 0` remains the normal-launch default.
  Both warning settings accept only `0` or `1`; invalid values repair to `0`.
- `UNBSELoader.exe --validate-only` still scans and reports plugin declarations
  and asset containers regardless of the two normal-launch warning toggles. It
  shows no popup and does not launch the game.
- Opt-in plugin and asset warnings now use a resizable native window with a
  read-only scrollable evidence pane. `Launch Anyway` and `Cancel` remain fixed
  at the bottom even for large mod lists, and the full report is also printed
  in the loader console.
- `General.bUseUObjectArrayCache` moves from a hard requirement to a safe
  default. The package still ships `false`, but the loader preserves either a
  valid lowercase `true` or `false` developer choice. Invalid or missing values
  repair to `false`.

The UObject array cache setting controls UE4SS's global UObject cache and
GUObjectArray create/delete listeners. Disabling it does not prevent normal
Unreal asset loading, UObject creation, UNBSE loading, or OBSE plugin loading.
Enabling it restores cache-dependent developer features such as affected Live
View operations, with the startup-stability tradeoff documented by UE4SS.

These warning changes do not establish compatibility for every native or asset
mod. Native plugin support remains plugin-by-plugin, the asset check remains a
bounded static preflight, and this candidate does not claim support for game
builds other than Steam `1.512.105.0`.

## Installation

Install `UNBSE-0.13.2-rc.1.zip` as the same single manual, Vortex, or Mod
Organizer 2 package described in the README. Remove the legacy OBSE64 loader
and runtime files first, preserve `OBSE/Plugins`, and remove a separate UE4SS
package before installing UNBSE.

This rapid-test package is intentionally unsigned to shorten the test cycle.
It is still rebuilt from pinned source, packaged with reproducibility inputs,
and accompanied by SHA-256 checksums.
