# UNBSE native SDK

UNBSE exposes small, versioned C interfaces for native add-ons:

- `UNBSEAddonHostV1.h` registers add-ons and owns their lifetime.
- `UNBSEMessagingV1.h` provides sender-filtered broadcast and targeted messages.
- `UNBSEScriptServiceV1.h` registers bounded functions for an attached UE4SS Lua VM.
- `UNBSERuntimeInfoV1.h` reports the loaded runtime and executable identity.
- `UNBSERelocationV1.h` resolves bounded executable RVAs for registered owners.

An add-on locates `UNBSE_QueryAddonHostV1`, requests ABI version 1, and submits an
`UNBSEAddonDescriptorV1`. Keep the returned owner handle for every later SDK call
and retire it during shutdown. Add-ons must declare runtime read, runtime write,
file I/O, and network I/O effects accurately.

The host reports missing optional capabilities but allows an unverified attempt.
Structural PE failures and invalid ownership are rejected. A bounded RVA is not
an Address Library relocation ID.

The base package also includes the clean-room `UNBSEOBSE64Interop` module. It
loads structurally valid x64 plugins from `OBSE/Plugins`, supplies the public
interfaces implemented by UNBSE, and emits the supported lifecycle messages.
Native plugin compatibility remains plugin-specific for game version
`1.512.105.0`.
