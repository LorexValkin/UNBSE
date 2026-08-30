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

The `0.11.0-rc.1` release targets only the current Steam executable,
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

1. If you have legacy OBSE64 installed, remove `obse64_loader.exe`,
   `obse64_steam_loader.dll`, and the matching `obse64_*.dll` runtime from the
   game's `Win64` directory before installing UNBSE.
2. Keep your existing `OBSE/Plugins` directory.
3. Extract `UNBSE-0.11.0-rc.1.zip` directly into:
   `Oblivion Remastered/OblivionRemastered/Binaries/Win64`.
4. Launch the game normally through Steam.

The archive is laid out for drag-and-drop installation. It contains the pinned
UE4SS runtime, the UNBSE core, the enabled OBSE64 interoperability module, SDK
headers, the package manifest, and checksums.

To remove UNBSE, delete `ue4ss/Mods/UNBSE` and
`ue4ss/Mods/UNBSEOBSE64Interop`. Remove `dwmapi.dll` and `ue4ss/UE4SS.dll` only
when no other installed mod uses UE4SS. Do not delete `OBSE/Plugins`.

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

The first command rebuilds the core and interoperability DLLs. The second creates
the drag-and-drop runtime archive, a production-source archive, and
`SHA256SUMS.txt`. UNBSE does not currently declare its own license; the bundled
UE4SS license remains included with its runtime.
