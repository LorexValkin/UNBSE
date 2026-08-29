#pragma once

// Stable native add-on discovery and ownership ABI for the UNBSE runtime
// capability injector. This ABI records declared effects and allocates an
// owner handle for the script-service registry. It does not provide an
// in-process security sandbox.
#include <UNBSEScriptServiceV1.h>

enum {
    UNBSE_ADDON_HOST_ABI_VERSION = 1u,
    UNBSE_ADDON_ID_BYTES = 64u,
    UNBSE_ADDON_VERSION_BYTES = 32u,
    UNBSE_ADDON_MAX_REGISTERED = 32u
};

typedef enum UNBSEAddonHostCapabilityV1 {
    UNBSE_ADDON_HOST_CAPABILITY_SCRIPT_SERVICE_V1 = 1u << 0,
    UNBSE_ADDON_HOST_CAPABILITY_MESSAGING_V1 = 1u << 1,
    UNBSE_ADDON_HOST_CAPABILITY_RUNTIME_INFO_V1 = 1u << 2,
    UNBSE_ADDON_HOST_CAPABILITY_RELOCATION_V1 = 1u << 3
} UNBSEAddonHostCapabilityV1;

typedef enum UNBSEAddonDeclaredEffectV1 {
    UNBSE_ADDON_EFFECT_RUNTIME_READ = 1u << 0,
    UNBSE_ADDON_EFFECT_RUNTIME_WRITE = 1u << 1,
    UNBSE_ADDON_EFFECT_FILE_IO = 1u << 2,
    UNBSE_ADDON_EFFECT_NETWORK_IO = 1u << 3
} UNBSEAddonDeclaredEffectV1;

typedef enum UNBSEAddonResultCodeV1 {
    UNBSE_ADDON_RESULT_OK = 0,
    UNBSE_ADDON_RESULT_UNSUPPORTED_HOST_VERSION = 1,
    UNBSE_ADDON_RESULT_MALFORMED_DESCRIPTOR = 2,
    UNBSE_ADDON_RESULT_DUPLICATE_ID = 3,
    UNBSE_ADDON_RESULT_REGISTRY_FULL = 4,
    UNBSE_ADDON_RESULT_UNKNOWN_OWNER = 5,
    UNBSE_ADDON_RESULT_OWNER_RETIRING = 6,
    UNBSE_ADDON_RESULT_RETIRE_TIMEOUT = 7,
    UNBSE_ADDON_RESULT_SHUTTING_DOWN = 8,
    UNBSE_ADDON_RESULT_SCRIPT_SERVICE_FAILURE = 9
} UNBSEAddonResultCodeV1;

typedef enum UNBSEAddonCompatibilityV1 {
    UNBSE_ADDON_COMPATIBILITY_VERIFIED = 0,
    UNBSE_ADDON_COMPATIBILITY_UNVERIFIED_ATTEMPT = 1
} UNBSEAddonCompatibilityV1;

typedef struct UNBSEAddonDescriptorV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t declaredEffects;
    uint32_t requiredHostCapabilities;
    char addonId[UNBSE_ADDON_ID_BYTES];
    char addonVersion[UNBSE_ADDON_VERSION_BYTES];
    uint64_t reserved[4];
} UNBSEAddonDescriptorV1;

typedef struct UNBSEAddonRegistrationV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t ownerHandle;
    uint32_t hostCapabilities;
    uint32_t missingHostCapabilities;
    uint32_t compatibility;
    uint64_t reserved[3];
} UNBSEAddonRegistrationV1;

typedef uint32_t (UNBSE_SCRIPT_CALL *UNBSEAddonGetHostCapabilitiesV1)(void);
typedef uint32_t (UNBSE_SCRIPT_CALL *UNBSEAddonRegisterV1)(
    const UNBSEAddonDescriptorV1* descriptor,
    UNBSEAddonRegistrationV1* registration);
typedef uint32_t (UNBSE_SCRIPT_CALL *UNBSEAddonRetireV1)(
    uint32_t ownerHandle,
    uint32_t deadlineMs);
typedef const char* (UNBSE_SCRIPT_CALL *UNBSEAddonResultNameV1)(uint32_t resultCode);

typedef struct UNBSEAddonHostV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    UNBSEAddonGetHostCapabilitiesV1 getHostCapabilities;
    UNBSEAddonRegisterV1 registerAddon;
    UNBSEAddonRetireV1 retireAddon;
    UNBSEAddonResultNameV1 resultName;
    UNBSEQueryScriptServiceV1Function queryScriptService;
    uint64_t reserved[3];
} UNBSEAddonHostV1;

typedef int32_t (UNBSE_SCRIPT_CALL *UNBSEQueryAddonHostV1Function)(
    uint32_t requestedVersion,
    UNBSEAddonHostV1* host);

#if defined(__cplusplus)
static_assert(sizeof(UNBSEAddonDescriptorV1) == 144, "UNBSE add-on descriptor ABI size drift");
static_assert(sizeof(UNBSEAddonRegistrationV1) == 48, "UNBSE add-on registration ABI size drift");
static_assert(sizeof(UNBSEAddonHostV1) == 72, "UNBSE add-on host ABI size drift");
#endif
