# UNBSE 0.13.0-rc.1

UNBSE 0.13.0-rc.1 is a release candidate for the Steam
`OblivionRemastered-Win64-Shipping.exe` runtime `1.512.105.0`. It contains the
following changes since 0.12.0.

## Compatibility changes

- The patched host can load the separately installed ConsoleUtils 1.1 binary
  from Nexus mod 5021 despite its August 2025 UE4SS C++ vtable layout. The
  compatibility path is gated by the exact `ConsoleUtils` name and `1.1`
  version; other C++ mods continue through the normal host path.
- ConsoleUtils' old EngineTick scan is satisfied through a one-shot host tick
  probe. This initializes its game-thread guard without asking the utility to
  hook UE4SS's existing EngineTick detour a second time.
- ConsoleUtils is not rebuilt, modified, signed, or included. Users install the
  author's original utility separately.

## Settings recovery

- `UNBSELoader.exe` now reviews `UE4SS-settings.ini` by the required section,
  key, and value instead of rejecting every customized file by exact hash.
- If an older INI is missing required entries or contains incompatible values,
  normal launch lists the differences and offers to update only those entries.
  Other settings and comments are preserved.
- Existing settings receive a uniquely named `.unbse-backup` through an atomic
  Windows file replacement. A repaired or newly packaged file carries the
  comment marker `UNBSE-Settings-Profile: 0.13.0-rc.1`.
- After a repair, the loader exits successfully without starting the game and
  asks the user to launch again from Vortex, MO2, or the normal launcher. It
  does not attempt to restart a mod manager or duplicate its virtual filesystem
  context. `--validate-only` remains read-only.

## Loader and release hardening

- Direct launch now lets the Windows process loader initialize before injecting
  the pinned UE4SS host. Injection is verified from the remote module list
  rather than the truncated 32-bit thread result of a 64-bit `LoadLibraryW`.
- The loader no longer adds `--disable-ue4ss` to the game command line. An
  explicit `UE4SS_MODS_PATHS` value remains an override, preventing a manager or
  controlled test path from being shadowed by stale physical mod copies.
- `UNBSELoader.exe`, the patched `UE4SS.dll`, the UNBSE core DLL, and the UNBSE
  OBSE64 interoperability DLL are timestamped and Authenticode-signed by
  Computer Works through Microsoft Artifact Signing. The unchanged upstream
  `dwmapi.dll` retains its upstream signature state.

## Installation and upgrade

Remove legacy `obse64_loader.exe`, `obse64_steam_loader.dll`, and the matching
`obse64_*.dll` runtime before installing. Preserve `OBSE/Plugins`.

For Vortex, remove the separate community UE4SS package (Nexus mod 32), disable
the legacy OBSE64 requirement if Vortex installed it, install
`UNBSE-0.13.0-rc.1.zip`, and make `UNBSELoader.exe` the primary tool. If the
settings repair prompt appears, accept or decline it explicitly; after an
accepted repair, launch the tool a second time.

For MO2, install the same archive, accept the Oblivion Remastered plugin's
automatic file-tree fix, and launch the loader from the physical `Root` tree.
Manual users extract the archive directly into the game's `Win64` directory.

Native support remains plugin-by-plugin. This candidate claims compatibility
only with runtime `1.512.105.0` and the interfaces documented in the bundled
SDK.
