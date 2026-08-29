#pragma once

// Stable in-process messaging ABI for independently built UNBSE add-ons.
// Message data is borrowed for the duration of the callback only. The host
// copies listener metadata, but never copies or retains message payloads.
#include <UNBSEScriptServiceV1.h>

enum {
    UNBSE_MESSAGING_ABI_VERSION = 1u,
    UNBSE_MESSAGE_SENDER_BYTES = 64u,
    UNBSE_MESSAGE_MAX_BYTES = 1048576u,
    UNBSE_MESSAGE_MAX_LISTENERS = 128u
};

typedef enum UNBSEMessagingResultCodeV1 {
    UNBSE_MESSAGING_RESULT_OK = 0,
    UNBSE_MESSAGING_RESULT_UNSUPPORTED_HOST_VERSION = 1,
    UNBSE_MESSAGING_RESULT_MALFORMED_REQUEST = 2,
    UNBSE_MESSAGING_RESULT_UNKNOWN_OWNER = 3,
    UNBSE_MESSAGING_RESULT_OWNER_RETIRING = 4,
    UNBSE_MESSAGING_RESULT_DUPLICATE_LISTENER = 5,
    UNBSE_MESSAGING_RESULT_LISTENER_REGISTRY_FULL = 6,
    UNBSE_MESSAGING_RESULT_NO_LISTENERS = 7,
    UNBSE_MESSAGING_RESULT_SHUTTING_DOWN = 8
} UNBSEMessagingResultCodeV1;

typedef enum UNBSECoreMessageTypeV1 {
    UNBSE_CORE_MESSAGE_RUNTIME_READY = 1,
    UNBSE_CORE_MESSAGE_LUA_READY = 2,
    UNBSE_CORE_MESSAGE_RUNTIME_STOPPING = 3
} UNBSECoreMessageTypeV1;

typedef struct UNBSEMessageV1 {
    uint32_t structSize;
    uint32_t messageType;
    uint32_t dataSize;
    uint32_t reserved;
    char senderId[UNBSE_MESSAGE_SENDER_BYTES];
    const void* data;
} UNBSEMessageV1;

typedef void (UNBSE_SCRIPT_CALL *UNBSEMessageCallbackV1)(const UNBSEMessageV1* message,
                                                         void* context);
typedef uint32_t (UNBSE_SCRIPT_CALL *UNBSEMessagingRegisterListenerV1)(
        uint32_t ownerHandle,
        const char* senderFilter,
        UNBSEMessageCallbackV1 callback,
        void* context);
typedef uint32_t (UNBSE_SCRIPT_CALL *UNBSEMessagingDispatchV1)(
        uint32_t senderOwnerHandle,
        uint32_t messageType,
        const void* data,
        uint32_t dataSize,
        const char* receiverId);
typedef const char* (UNBSE_SCRIPT_CALL *UNBSEMessagingResultNameV1)(uint32_t resultCode);

typedef struct UNBSEMessagingV1 {
    uint32_t structSize;
    uint32_t apiVersion;
    UNBSEMessagingRegisterListenerV1 registerListener;
    UNBSEMessagingDispatchV1 dispatch;
    UNBSEMessagingResultNameV1 resultName;
    uint64_t reserved[3];
} UNBSEMessagingV1;

typedef int32_t (UNBSE_SCRIPT_CALL *UNBSEQueryMessagingV1Function)(
        uint32_t requestedVersion,
        UNBSEMessagingV1* messaging);

#if defined(__cplusplus)
static_assert(sizeof(UNBSEMessageV1) == 88, "UNBSE message ABI size drift");
static_assert(sizeof(UNBSEMessagingV1) == 56, "UNBSE messaging ABI size drift");
#endif
