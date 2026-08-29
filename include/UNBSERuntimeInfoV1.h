#pragma once

// Stable runtime-identity ABI for independently built UNBSE add-ons.
// This service reports what is loaded; it does not claim that an executable is
// compatible with an add-on. Add-ons remain responsible for version-specific
// address and behavior checks.
#include <UNBSEScriptServiceV1.h>

enum {
    UNBSE_RUNTIME_INFO_ABI_VERSION = 1u,
    UNBSE_RUNTIME_HOST_VERSION_BYTES = 32u,
    UNBSE_RUNTIME_FOUNDATION_ID_BYTES = 128u,
    UNBSE_RUNTIME_EXECUTABLE_NAME_BYTES = 128u
};

typedef enum UNBSERuntimePlatformV1 {
    UNBSE_RUNTIME_PLATFORM_WINDOWS_X64 = 1
} UNBSERuntimePlatformV1;

typedef enum UNBSERuntimeIdentityFlagV1 {
    UNBSE_RUNTIME_IDENTITY_PROCESS_OBSERVED = 1u << 0,
    UNBSE_RUNTIME_IDENTITY_PE_OBSERVED = 1u << 1,
    UNBSE_RUNTIME_IDENTITY_FOUNDATION_DECLARED = 1u << 2
} UNBSERuntimeIdentityFlagV1;

typedef struct UNBSERuntimeInfoV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t platform;
    uint32_t identityFlags;
    uint32_t processId;
    uint32_t peTimestamp;
    uint32_t peChecksum;
    uint32_t peMachine;
    uint64_t imageBase;
    uint64_t imageSize;
    char hostVersion[UNBSE_RUNTIME_HOST_VERSION_BYTES];
    char foundationId[UNBSE_RUNTIME_FOUNDATION_ID_BYTES];
    char executableName[UNBSE_RUNTIME_EXECUTABLE_NAME_BYTES];
    uint64_t reserved[4];
} UNBSERuntimeInfoV1;

typedef int32_t (UNBSE_SCRIPT_CALL *UNBSEQueryRuntimeInfoV1Function)(
        uint32_t requestedVersion,
        UNBSERuntimeInfoV1* runtimeInfo);

#if defined(__cplusplus)
static_assert(sizeof(UNBSERuntimeInfoV1) == 368, "UNBSE runtime-info ABI size drift");
#endif
