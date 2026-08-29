#pragma once

// Stable executable-RVA resolution ABI for independently built UNBSE add-ons.
// The caller selects an RVA for the target runtime identity. The host validates
// ownership, image bounds, and arithmetic; callers remain responsible for
// selecting an address compatible with their target game build.
#include <UNBSEScriptServiceV1.h>

enum {
    UNBSE_RELOCATION_ABI_VERSION = 1u
};

typedef enum UNBSERelocationModuleV1 {
    UNBSE_RELOCATION_MODULE_EXECUTABLE = 1
} UNBSERelocationModuleV1;

typedef enum UNBSERelocationResultCodeV1 {
    UNBSE_RELOCATION_RESULT_OK = 0,
    UNBSE_RELOCATION_RESULT_UNSUPPORTED_HOST_VERSION = 1,
    UNBSE_RELOCATION_RESULT_MALFORMED_REQUEST = 2,
    UNBSE_RELOCATION_RESULT_UNKNOWN_OWNER = 3,
    UNBSE_RELOCATION_RESULT_OWNER_RETIRING = 4,
    UNBSE_RELOCATION_RESULT_UNSUPPORTED_MODULE = 5,
    UNBSE_RELOCATION_RESULT_OUT_OF_RANGE = 6,
    UNBSE_RELOCATION_RESULT_SHUTTING_DOWN = 7,
    UNBSE_RELOCATION_RESULT_RUNTIME_UNAVAILABLE = 8
} UNBSERelocationResultCodeV1;

typedef struct UNBSERelocationRequestV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t ownerHandle;
    uint32_t module;
    uint64_t relativeAddress;
    uint64_t minimumBytes;
    uint64_t reserved[2];
} UNBSERelocationRequestV1;

typedef struct UNBSERelocationResultV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    uint64_t absoluteAddress;
    uint64_t imageBase;
    uint64_t imageSize;
    uint64_t relativeAddress;
    uint64_t reserved[2];
} UNBSERelocationResultV1;

typedef uint32_t (UNBSE_SCRIPT_CALL *UNBSERelocationResolveV1)(
        const UNBSERelocationRequestV1* request,
        UNBSERelocationResultV1* result);
typedef const char* (UNBSE_SCRIPT_CALL *UNBSERelocationResultNameV1)(uint32_t resultCode);

typedef struct UNBSERelocationV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    UNBSERelocationResolveV1 resolve;
    UNBSERelocationResultNameV1 resultName;
    uint64_t reserved[3];
} UNBSERelocationV1;

typedef int32_t (UNBSE_SCRIPT_CALL *UNBSEQueryRelocationV1Function)(
        uint32_t requestedVersion,
        UNBSERelocationV1* relocation);

#if defined(__cplusplus)
static_assert(sizeof(UNBSERelocationRequestV1) == 48, "UNBSE relocation request ABI size drift");
static_assert(sizeof(UNBSERelocationResultV1) == 56, "UNBSE relocation result ABI size drift");
static_assert(sizeof(UNBSERelocationV1) == 48, "UNBSE relocation service ABI size drift");
#endif
