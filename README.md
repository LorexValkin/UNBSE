# UNBSE

UNBSE is the Unblivion Script Extender for Oblivion Remastered. Its goal is to
provide a small, open native extension layer for mods: plugin loading, lifecycle
events, messaging, runtime information, executable trampolines, and versioned SDK
interfaces.

## Build a plugin

UNBSE is designed to be extended. Plugin developers can use the public,
versioned C headers in [`include/`](include/) to register a native add-on and
request only the runtime services they need.

- [Plugin development guide](docs/plugin-development.html)
- [Versioned documentation API](docs/api/index.json)
- [Current SDK interface catalog](docs/api/v1/interfaces.json)

The documentation is kept with the source and is published as a static website
from `main`, so the guide and machine-readable API describe the shipped SDK.

The `0.13.0-rc.1` release candidate targets only the current Steam executable,
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

UNBSE `0.13.0-rc.1` targets only the Steam executable version `1.512.105.0`.
The packaged UE4SS console is intentionally visible so its startup log, loaded
mods, and UNBSE compatibility messages are immediately observable. The same log
is retained in `ue4ss/UE4SS.log`.

Before any install, remove legacy `obse64_loader.exe`,
`obse64_steam_loader.dll`, and the matching `obse64_*.dll` runtime from the
game's `Win64` directory. Keep the existing `OBSE/Plugins` directory.

### One archive: manual, Vortex, or MO2

Use the single `UNBSE-0.13.0-rc.1.zip` archive. For a manual install, extract it
directly into `Oblivion Remastered/OblivionRemastered/Binaries/Win64`. Launch
`UNBSELoader.exe` when you want the prelaunch plugin-version check; the normal
Steam launch remains available after the settings preflight. On first launch
after an upgrade, the loader checks the active `UE4SS-settings.ini` for every
UNBSE-required key. If an older file is missing keys or contains incompatible
values, it lists the differences and offers to repair only those keys. An
existing file receives a uniquely named `.unbse-backup` before replacement;
unrelated settings and comments are preserved. After a repair, the loader exits
without starting the game and asks you to launch it again from Vortex, MO2, or
your normal launcher. `--validate-only` reports the same differences without
changing the file. The packaged and repaired INI also contains the comment
marker `UNBSE-Settings-Profile: 0.13.0-rc.1` for identification.

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

1. Install the same `UNBSE-0.13.0-rc.1.zip` as one MO2 mod, accept the Oblivion
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

### Plugin-version warning

Before creating the game process, `UNBSELoader.exe` reads each DLL declaration
in `OBSE/Plugins` without loading or executing the DLL. A grouped
`Invalid Version Mod` warning appears when a plugin declares only other game
versions, claims version independence without naming `1.512.105.0`, has an
unreadable declaration, or lacks its required load export. The warning shows the
plugin name, author, declaration class, filename, and declared game versions.

No is the default and cancels launch. Yes continues and remembers the exact game
and plugin SHA-256 pair, so the same warning does not require repeated input;
changing the game or plugin binary causes another check. This is an early-risk
warning, not proof that a plugin will crash or that an explicitly declared
plugin is safe. Use `UNBSELoader.exe --validate-only` to print every current
warning without showing a popup or launching the game. The acceptance file is
stored under `%LOCALAPPDATA%\UNBSE`.

### Asset-container warning

The same prelaunch pass recursively checks the `.pak`, `.utoc`, and `.ucas`
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

This is a container-envelope and top-level serialization-header check. It does
not deserialize every cooked package, validate every Unreal custom version or
asset schema, or prove that a mod is behaviorally compatible. Use
`UNBSELoader.exe --validate-only` to print the results without UI or launch.

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
`UE4SS.CppModLifecycle` diagnostic boundary.
When signing inputs are supplied, those four UNBSE-produced or patched binaries
receive verified, timestamped Authenticode signatures before their final hashes
are recorded. The unchanged upstream `dwmapi.dll` proxy retains its upstream
signature state.
The second creates one universal install archive, one production-source archive,
and `SHA256SUMS.txt`. UNBSE does not currently declare its own license; the
bundled UE4SS license remains included with its runtime.
