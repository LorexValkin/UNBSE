# UNBSE

UNBSE is the Unblivion Script Extender for Oblivion Remastered. Its goal is to
provide a small, open native extension layer for mods: plugin loading, lifecycle
events, messaging, runtime information, executable trampolines, and versioned SDK
interfaces.

## Current candidate

UNBSE `0.14.1` is the current signed prerelease candidate for Steam
runtime `1.512.105.0`.

- The published artifacts carry timestamped Computer Works signatures on every
  produced or patched PE. Local unsigned packages remain engineering test
  material. Real in-game testing is still required before the candidate may be
  called validated or promoted from prerelease.
- Legacy 11-slot UE4SS C++ mods are handled through a name-agnostic ABI adapter.
  ConsoleUtils `1.1` additionally uses an exact EngineTick compatibility rule;
  UNBSE does not package, rebuild, modify, or re-sign third-party mods.
- Legacy Lua calls to `KismetSystemLibrary.ExecuteConsoleCommand` using the
  historical three-argument shape receive only the omitted trailing value.
  Other UFunctions and argument-count mismatches retain strict validation.
- `UE4SS-settings.ini` exposes independent plugin-version and asset-container
  warning toggles; both ship off. Opt-in warnings use a resizable dialog with a
  scrollable evidence pane, while `--validate-only` remains a no-popup report.
- `General.bUseUObjectArrayCache` ships `false` for startup stability, but the
  loader now preserves a developer's explicit `true` or `false` choice.
- The loader detects incompatible older `UE4SS-settings.ini` files and can
  repair only the required keys after creating a backup. It then exits and
  asks the user to restart through Vortex, MO2, or their normal launcher.
- One archive supports manual installation, Vortex, and Mod Organizer 2 without
  overwriting `mods.txt`, `mods.json`, or third-party mod activation state.

See the [0.14.1 release notes](docs/release-notes-0.14.1.md) for the
complete compatibility and verification details.

## Build a plugin

UNBSE is designed to be extended. Plugin developers can use the public,
versioned C headers in [`include/`](include/) to register a native add-on and
request only the runtime services they need.

- [Plugin development guide](docs/plugin-development.html)
- [Versioned documentation API](docs/api/index.json)
- [Current SDK interface catalog](docs/api/v1/interfaces.json)

The documentation is kept with the source and is published as a static website
from `main`, so the guide and machine-readable API describe the shipped SDK.

The `0.14.1` prerelease candidate targets only the current Steam executable,
`OblivionRemastered-Win64-Shipping.exe` version `1.512.105.0`. UNBSE includes its
clean-room OBSE64 interoperability module and discovers compatible native plugins
from `OBSE/Plugins`. Compatibility is determined plugin by plugin; support for
older game versions or arbitrary OBSE64 plugins is not claimed.

MagicLoader 2 data mods are compatible with UNBSE. MagicLoader 2 manages its
generated data archives separately from UNBSE and can launch the normal shipping
executable with UNBSE installed. If a MagicLoader 2 mod also includes a native
OBSE64 plugin, that DLL remains subject to UNBSE's plugin-by-plugin compatibility
limits.

## Install

UNBSE `0.14.1` targets only the Steam executable version `1.512.105.0`.
The packaged UE4SS console is intentionally visible so its startup log, loaded
mods, and UNBSE compatibility messages are immediately observable. The same log
is retained in `ue4ss/UE4SS.log`.

Before any install, remove legacy `obse64_loader.exe`,
`obse64_steam_loader.dll`, and the matching `obse64_*.dll` runtime from the
game's `Win64` directory. Keep the existing `OBSE/Plugins` directory.

### One archive: manual, Vortex, or MO2

Use the single `UNBSE-0.14.1.zip` archive. For a manual install, extract it
directly into `Oblivion Remastered/OblivionRemastered/Binaries/Win64`. Launch
`UNBSELoader.exe` for settings validation and launch; normal launch skips both
optional warning scans with the packaged defaults. On first launch
after an upgrade, the loader checks the active `UE4SS-settings.ini` for every
UNBSE-required key. If an older file is missing keys or contains incompatible
values, it lists the differences and offers to repair only those keys. An
existing file receives a uniquely named `.unbse-backup` before replacement;
unrelated settings and comments are preserved. After a repair, the loader exits
without starting the game and asks you to launch it again from Vortex, MO2, or
your normal launcher. `--validate-only` reports the same differences without
changing the file. The packaged and repaired INI also contains the comment
marker `UNBSE-Settings-Profile: 0.14.1` for identification.

For Vortex, install and enable the same zip,
remove the separate Nexus UE4SS package (mod 32) if present, disable Vortex's
automatically installed legacy OBSE64 requirement, add `UNBSELoader.exe` as a
tool, and make that tool primary.

The drop-in archive includes the pinned UE4SS runtime, its standard Blueprint
loader modules, the UNBSE core and interoperability module, SDK headers,
`UNBSELoader.exe`, the package manifest, and checksums. It does not ship
`mods.txt` or `mods.json`, so it does not overwrite a user's or manager's mod
state. This is the only install archive published for the release.

### Mod Organizer 2

1. Install the same `UNBSE-0.14.1.zip` as one MO2 mod, accept the Oblivion
   Remastered plugin's automatic file-tree fix, and enable it. The plugin moves
   the complete self-contained payload under `Root`; do not use Root Builder,
   rename `dwmapi.dll`, or configure a force-loaded library.
2. Add this installed file as an MO2 executable:
   `Root/OblivionRemastered/Binaries/Win64/UNBSELoader.exe`. No arguments or
   custom working directory are required. The launcher locates the Steam game,
   verifies the pinned runtime, waits for USVFS, and then loads UE4SS.
3. Launch the new `UNBSE` executable from MO2. Its sibling runtime and UNBSE
   modules load from the physical `Root` payload, while the game process keeps
   MO2's virtualized mod view. The patched host starts its two bundled foundation
   modules independently of manager-controlled `enabled.txt` or `mods.txt`
   state, so no UE4SS tab setup is required.

Under MO2, the log is retained beside the physical runtime under the installed
mod's `Root` tree. The visible UE4SS console is the quickest check: it should
show both `UNBSE` C++ mods starting. `UNBSELoader.exe --validate-only` performs
discovery and hash checks without launching the game.

### Verify the active installation

The upstream banner remains `UE4SS - v3.0.1 Beta #0 - Git SHA #68dd45c`
because UNBSE builds from that pinned source revision. The banner alone cannot
distinguish the patched host from the community package. A successful UNBSE
startup also logs marker-independent starts for `UNBSE` and
`UNBSEOBSE64Interop`, followed by `UE4SS.CppModLifecycle` and `UNBSE` records.
If startup reaches `Event loop start` without those records, the active
deployment is not running the UNBSE modules.

### Plugin-version diagnostic

The packaged and repaired `UE4SS-settings.ini` contains this user-controlled
setting, which defaults to off:

```ini
[UNBSE]
EnablePluginVersionWarning = 0
EnableAssetContainerWarning = 0
```

With `0`, normal `UNBSELoader.exe` launches do not scan DLL declarations in
`OBSE/Plugins`, show an `Invalid Version Mod` list, ask for consent, or remember
plugin-warning choices. Set it to `1` to restore the warning, launch gate, and
per-binary acceptance behavior. Values other than `0` or `1` are repaired to
the off default. Native plugin compatibility remains plugin-by-plugin and is
not guaranteed.

`UNBSELoader.exe --validate-only` retains the opt-in static report for authors
and troubleshooting regardless of the setting. It reads declarations without
loading or executing the DLLs, prints findings without a popup, and does not
launch the game.

When enabled, the plugin warning uses a resizable native window with a
scrollable evidence pane and fixed `Launch Anyway` and `Cancel` buttons. Large
mod lists therefore cannot push the decision buttons off-screen. The complete
report is also printed in the loader console.

### Asset-container warning

`EnableAssetContainerWarning = 0` makes normal launches skip the asset-container
scan, popup, launch gate, and acceptance-state update. Set it to `1` to opt into
the warning. Values other than `0` or `1` are repaired to the off default.

When enabled, the prelaunch pass recursively checks the `.pak`, `.utoc`, and `.ucas`
files visible under `OblivionRemastered/Content/Paks`, including files deployed
by Vortex or exposed inside MO2's virtual game tree. It verifies IoStore magic,
table bounds, container identity, required UCAS partitions, and the current
retail TOC layout. For an uncompressed container-header chunk it also verifies
the serialized `NoExportInfo` container-header version and identity. Pak files
are checked for the current footer version, bounded index, and matching index
SHA-1.

An `Invalid Asset Container` warning appears before launch when that static
evidence is malformed or differs from the current retail format. No is the
default. Yes remembers the current game and container evidence in a separate
file under `%LOCALAPPDATA%\UNBSE`; changed evidence is checked again. Containers
whose header or pak index cannot be inspected within the bounded preflight are
reported as `envelope-only`, not rejected merely for being unverifiable.

The opt-in asset warning uses the same resizable, scrollable window as the
plugin warning, so every finding remains reviewable while the decision buttons
stay fixed at the bottom.

This is a container-envelope and top-level serialization-header check. It does
not deserialize every cooked package, validate every Unreal custom version or
asset schema, or prove that a mod is behaviorally compatible. Use
`UNBSELoader.exe --validate-only` to print the results without UI or launch.

### Developer UObject array cache

The package uses this safe launch default:

```ini
[General]
bUseUObjectArrayCache = false
```

`false` disables UE4SS's global UObject cache and GUObjectArray create/delete
listeners. It does not stop Unreal asset loading, UObject creation, UNBSE, or
OBSE plugin loading; it avoids an early-startup cache/listener path that UE4SS
itself notes can contribute to startup crashes. Some developer tooling—notably
cache-dependent Live View operations—requires the cache. Developers can set the
value to `true`; the loader now preserves either valid lowercase `true` or
`false` choice across launches and repairs any other value to `false`.

To remove UNBSE, delete `ue4ss/Mods/UNBSE` and
`ue4ss/Mods/UNBSEOBSE64Interop`. Remove `dwmapi.dll` and `ue4ss/UE4SS.dll` only
when no other installed mod uses UE4SS, and remove `UNBSELoader.exe`. Do not
delete `OBSE/Plugins`.

## A small native add-on

Include `UNBSEAddonHostV1.h`, locate the `UNBSE_QueryAddonHostV1` export in the
loaded process, and register a versioned descriptor:

```cpp
#include <UNBSEAddonHostV1.h>
#include <cstdint>
#include <cstring>

bool RegisterAddon(UNBSEQueryAddonHostV1Function query, std::uint32_t& owner)
{
    UNBSEAddonHostV1 host{};
    host.structSize = sizeof(host);
    if (!query(UNBSE_ADDON_HOST_ABI_VERSION, &host)) {
        return false;
    }

    UNBSEAddonDescriptorV1 descriptor{};
    descriptor.structSize = sizeof(descriptor);
    descriptor.apiVersion = UNBSE_ADDON_HOST_ABI_VERSION;
    descriptor.declaredEffects = UNBSE_ADDON_EFFECT_RUNTIME_READ;
    std::memcpy(descriptor.addonId, "example.weather", 16);
    std::memcpy(descriptor.addonVersion, "1.0.0", 6);

    UNBSEAddonRegistrationV1 registration{};
    registration.structSize = sizeof(registration);
    registration.apiVersion = UNBSE_ADDON_HOST_ABI_VERSION;
    if (host.registerAddon(&descriptor, &registration) != UNBSE_ADDON_RESULT_OK) {
        return false;
    }

    owner = registration.ownerHandle;
    return true;
}
```

Retain the returned host and owner handle while the add-on is active, and call
`host.retireAddon(owner, deadlineMs)` during shutdown. The other SDK headers add
messaging, script services, runtime identity, and bounded RVA resolution.

## Credits

- Accredited Tester: SirNwah

## Build and package

The build is pinned to UE4SS commit
`68dd45cb5630bd7745310c6630b19c14345176a2`; the manifest records its submodules,
patches, source hashes, and expected artifacts. From a Visual Studio x64 C++
environment:

```powershell
& .\ue4ss\scripts\Build-UNBSEUE4SSMod.ps1
& .\ue4ss\scripts\Package-UNBSERelease.ps1 `
    -FoundationArchivePath .\out\foundation\UE4SS_v3.0.1-1008-g68dd45cb.zip
```

For a public signed build, pass a compatible Windows SDK `signtool.exe`, the
Microsoft Artifact Signing client dlib, and a metadata file naming the signing
account and certificate profile:

```powershell
& .\ue4ss\scripts\Build-UNBSEUE4SSMod.ps1 `
    -SignToolPath '<Windows SDK>\x64\signtool.exe' `
    -ArtifactSigningDlibPath '<Artifact Signing client>\bin\x64\Azure.CodeSigning.Dlib.dll' `
    -ArtifactSigningMetadataPath '<private build inputs>\metadata.json'
```

The first command rebuilds the patched UE4SS host, core, interoperability DLL,
and native launcher. The package manifest pins that exact host and the launcher
will reject a substituted host, ensuring the shipped runtime contains the
`UE4SS.CppModLifecycle` diagnostic boundary and the bounded legacy Lua adapter.
When signing inputs are supplied, those four UNBSE-produced or patched binaries
receive verified, timestamped Authenticode signatures before their final hashes
are recorded. The unchanged upstream `dwmapi.dll` proxy retains its upstream
signature state.
The second creates one universal install archive, one production-source archive,
and `SHA256SUMS.txt`. UNBSE does not currently declare its own license; the
bundled UE4SS license remains included with its runtime.
