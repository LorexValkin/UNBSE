#pragma once

// UNBSE script-service ABI. This is not an OBSE compatibility surface.
// All pointers in this ABI are valid only for the duration of the synchronous call.
#include <stdint.h>

#ifdef _WIN32
#define UNBSE_SCRIPT_CALL __cdecl
#define UNBSE_SCRIPT_EXPORT __declspec(dllexport)
#else
#define UNBSE_SCRIPT_CALL
#define UNBSE_SCRIPT_EXPORT
#endif

enum {
    UNBSE_SCRIPT_SERVICE_ABI_VERSION = 1u,
    UNBSE_SCRIPT_TOKEN_BYTES = 32u,
    UNBSE_SCRIPT_UTF8_BYTES = 256u,
    UNBSE_SCRIPT_DIAGNOSTIC_BYTES = 128u,
    UNBSE_SCRIPT_MAX_ARGUMENTS = 8u,
    UNBSE_SCRIPT_MAX_FUNCTIONS = 64u,
    UNBSE_SCRIPT_MAX_FUNCTIONS_PER_OWNER = 16u,
    UNBSE_SCRIPT_DEFAULT_DEADLINE_MS = 50u
};

typedef enum UNBSEScriptCapabilityV1 {
    UNBSE_SCRIPT_CAPABILITY_DECLARATION_REGISTRY = 1u << 0,
    UNBSE_SCRIPT_CAPABILITY_UE4SS_LUA_VM = 1u << 1,
    UNBSE_SCRIPT_CAPABILITY_GAME_SCRIPT_VM = 1u << 2,
    UNBSE_SCRIPT_CAPABILITY_TES_IDENTITY_SNAPSHOT = 1u << 3,
    UNBSE_SCRIPT_CAPABILITY_GUID_XAAG_HANDLERS = 1u << 4,
    UNBSE_SCRIPT_CAPABILITY_AREA_RUNTIME_SNAPSHOT = 1u << 5,
    UNBSE_SCRIPT_CAPABILITY_AREA_SPLINE_SNAPSHOT = 1u << 6,
    UNBSE_SCRIPT_CAPABILITY_MATERIAL_PARAMETER_COLLECTIONS = 1u << 7,
    UNBSE_SCRIPT_CAPABILITY_RVT_PRODUCERS = 1u << 8
} UNBSEScriptCapabilityV1;

typedef enum UNBSEScriptValueTypeV1 {
    UNBSE_SCRIPT_VALUE_NONE = 0,
    UNBSE_SCRIPT_VALUE_BOOL = 1,
    UNBSE_SCRIPT_VALUE_INT64 = 2,
    UNBSE_SCRIPT_VALUE_FLOAT64 = 3,
    UNBSE_SCRIPT_VALUE_UTF8 = 4
} UNBSEScriptValueTypeV1;

typedef enum UNBSEScriptResultCodeV1 {
    UNBSE_SCRIPT_RESULT_OK = 0,
    UNBSE_SCRIPT_RESULT_UNSUPPORTED_SERVICE_VERSION = 1,
    UNBSE_SCRIPT_RESULT_MALFORMED_DECLARATION = 2,
    UNBSE_SCRIPT_RESULT_NAMESPACE_VIOLATION = 3,
    UNBSE_SCRIPT_RESULT_DUPLICATE_NAME = 4,
    UNBSE_SCRIPT_RESULT_REGISTRY_FULL = 5,
    UNBSE_SCRIPT_RESULT_OWNER_LIMIT = 6,
    UNBSE_SCRIPT_RESULT_OWNER_RETIRED = 7,
    UNBSE_SCRIPT_RESULT_VM_UNAVAILABLE = 8,
    UNBSE_SCRIPT_RESULT_VM_THREAD_MISMATCH = 9,
    UNBSE_SCRIPT_RESULT_FUNCTION_NOT_FOUND = 10,
    UNBSE_SCRIPT_RESULT_ARGUMENT_MISMATCH = 11,
    UNBSE_SCRIPT_RESULT_CALLBACK_FAILED = 12,
    UNBSE_SCRIPT_RESULT_DEADLINE_EXPIRED = 13,
    UNBSE_SCRIPT_RESULT_RETIRE_TIMEOUT = 14,
    UNBSE_SCRIPT_RESULT_SHUTTING_DOWN = 15,
    UNBSE_SCRIPT_RESULT_NAMESPACE_COLLISION = 16
} UNBSEScriptResultCodeV1;

typedef enum UNBSEScriptFunctionFlagsV1 {
    UNBSE_SCRIPT_FUNCTION_READ_ONLY = 1u << 0
} UNBSEScriptFunctionFlagsV1;

typedef struct UNBSEScriptValueV1 {
    uint32_t structSize;
    uint32_t type;
    int64_t integerValue;
    double numberValue;
    char utf8Value[UNBSE_SCRIPT_UTF8_BYTES];
    uint64_t reserved[1];
} UNBSEScriptValueV1;

typedef struct UNBSEScriptInvocationV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t argumentCount;
    uint32_t deadlineMs;
    uint32_t windowsThreadId;
    uint32_t reserved;
    uint64_t sequence;
    const UNBSEScriptValueV1* arguments;
} UNBSEScriptInvocationV1;

typedef struct UNBSEScriptResultV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t code;
    uint32_t reserved;
    UNBSEScriptValueV1 value;
    char diagnostic[UNBSE_SCRIPT_DIAGNOSTIC_BYTES];
} UNBSEScriptResultV1;

typedef void (UNBSE_SCRIPT_CALL *UNBSEScriptFunctionV1)(
    const UNBSEScriptInvocationV1* invocation,
    UNBSEScriptResultV1* result,
    void* context);

typedef struct UNBSEScriptFunctionDeclarationV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t flags;
    uint32_t argumentCount;
    uint32_t resultType;
    uint32_t reserved;
    char namespaceToken[UNBSE_SCRIPT_TOKEN_BYTES];
    char functionToken[UNBSE_SCRIPT_TOKEN_BYTES];
    uint32_t argumentTypes[UNBSE_SCRIPT_MAX_ARGUMENTS];
    UNBSEScriptFunctionV1 callback;
    void* context;
    uint64_t reserved2[2];
} UNBSEScriptFunctionDeclarationV1;

typedef uint32_t (UNBSE_SCRIPT_CALL *UNBSEScriptGetCapabilitiesV1)(void);
typedef uint32_t (UNBSE_SCRIPT_CALL *UNBSEScriptRegisterFunctionV1)(
    uint32_t ownerHandle,
    const UNBSEScriptFunctionDeclarationV1* declaration);
typedef uint32_t (UNBSE_SCRIPT_CALL *UNBSEScriptRetireOwnerV1)(
    uint32_t ownerHandle,
    uint32_t deadlineMs);

typedef struct UNBSEScriptServiceV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    UNBSEScriptGetCapabilitiesV1 getCapabilities;
    UNBSEScriptRegisterFunctionV1 registerFunction;
    UNBSEScriptRetireOwnerV1 retireOwner;
    uint64_t reserved[5];
} UNBSEScriptServiceV1;

typedef int32_t (UNBSE_SCRIPT_CALL *UNBSEQueryScriptServiceV1Function)(
    uint32_t requestedVersion,
    UNBSEScriptServiceV1* service);

#if defined(__cplusplus)
static_assert(sizeof(UNBSEScriptValueV1) == 288, "UNBSE script ABI size drift");
static_assert(sizeof(UNBSEScriptInvocationV1) == 40, "UNBSE script ABI size drift");
static_assert(sizeof(UNBSEScriptResultV1) == 432, "UNBSE script ABI size drift");
static_assert(sizeof(UNBSEScriptFunctionDeclarationV1) == 152, "UNBSE script ABI size drift");
static_assert(sizeof(UNBSEScriptServiceV1) == 72, "UNBSE script ABI size drift");
#endif
