#pragma once

#include <UNBSEAddonHostV1.h>
#include <UNBSEMessagingV1.h>
#include <UNBSERelocationV1.h>
#include <UNBSERuntimeInfoV1.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

namespace RC::UNBSE
{
    class FScriptServiceRegistry;
    using FAddonRegistryEventSink = std::function<void(const std::string&)>;

    struct FAddonRuntimeIdentity
    {
        std::string HostVersion{};
        std::string FoundationId{};
    };

    class FAddonRegistry final
    {
      public:
        explicit FAddonRegistry(FScriptServiceRegistry& ScriptRegistry,
                                FAddonRegistryEventSink EventSink = {},
                                FAddonRuntimeIdentity RuntimeIdentity = {});
        ~FAddonRegistry();
        FAddonRegistry(const FAddonRegistry&) = delete;
        FAddonRegistry& operator=(const FAddonRegistry&) = delete;

        auto Register(const UNBSEAddonDescriptorV1* Descriptor,
                      UNBSEAddonRegistrationV1* Registration) -> std::uint32_t;
        auto Retire(std::uint32_t OwnerHandle, std::chrono::milliseconds Timeout) -> std::uint32_t;
        auto RegisterListener(std::uint32_t OwnerHandle,
                              const char* SenderFilter,
                              UNBSEMessageCallbackV1 Callback,
                              void* Context) -> std::uint32_t;
        auto Dispatch(std::uint32_t SenderOwnerHandle,
                      std::uint32_t MessageType,
                      const void* Data,
                      std::uint32_t DataSize,
                      const char* ReceiverId) -> std::uint32_t;
        auto DispatchCore(std::uint32_t MessageType, const void* Data, std::uint32_t DataSize)
                -> std::uint32_t;
        auto BeginShutdown(std::chrono::milliseconds Timeout) -> std::uint32_t;
        auto Capabilities() const -> std::uint32_t;
        auto RuntimeInfo(UNBSERuntimeInfoV1* Info) const -> bool;
        auto ResolveExecutableRva(const UNBSERelocationRequestV1* Request,
                                  UNBSERelocationResultV1* Result) -> std::uint32_t;
        auto AddonCount() const -> std::size_t;

      private:
        struct FEntry
        {
            bool Occupied{};
            bool Retiring{};
            std::uint32_t OwnerHandle{};
            std::uint32_t DeclaredEffects{};
            std::string AddonId{};
            std::string AddonVersion{};
        };

        struct FListener
        {
            bool Occupied{};
            std::uint32_t OwnerHandle{};
            std::string SenderFilter{};
            UNBSEMessageCallbackV1 Callback{};
            void* Context{};
            std::size_t ActiveCalls{};
        };

        auto FindOwnerLocked(std::uint32_t OwnerHandle) -> FEntry*;
        auto OwnerMessageCallsLocked(std::uint32_t OwnerHandle) const -> std::size_t;
        auto RemoveOwnerListenersLocked(std::uint32_t OwnerHandle) -> void;

        FScriptServiceRegistry& m_script_registry;
        FAddonRegistryEventSink m_event_sink{};
        FAddonRuntimeIdentity m_runtime_identity{};
        mutable std::mutex m_mutex{};
        std::array<FEntry, UNBSE_ADDON_MAX_REGISTERED> m_entries{};
        std::array<FListener, UNBSE_MESSAGE_MAX_LISTENERS> m_listeners{};
        std::condition_variable m_changed{};
        std::uint32_t m_next_owner_handle{2};
        std::size_t m_addon_count{};
        bool m_shutting_down{};
    };

    auto PublishAddonRegistry(FAddonRegistry* Registry) -> bool;
    auto UnpublishAddonRegistry(FAddonRegistry* Registry) -> void;
    auto GetPublishedAddonRegistry() -> FAddonRegistry*;
} // namespace RC::UNBSE

extern "C" UNBSE_SCRIPT_EXPORT int32_t UNBSE_SCRIPT_CALL UNBSE_QueryAddonHostV1(
        uint32_t RequestedVersion,
        UNBSEAddonHostV1* Host);

extern "C" UNBSE_SCRIPT_EXPORT int32_t UNBSE_SCRIPT_CALL UNBSE_QueryMessagingV1(
        uint32_t RequestedVersion,
        UNBSEMessagingV1* Messaging);

extern "C" UNBSE_SCRIPT_EXPORT int32_t UNBSE_SCRIPT_CALL UNBSE_QueryRuntimeInfoV1(
        uint32_t RequestedVersion,
        UNBSERuntimeInfoV1* RuntimeInfo);

extern "C" UNBSE_SCRIPT_EXPORT int32_t UNBSE_SCRIPT_CALL UNBSE_QueryRelocationV1(
        uint32_t RequestedVersion,
        UNBSERelocationV1* Relocation);
