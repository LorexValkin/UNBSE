#include <AddonRegistry.hpp>

#include <ScriptServiceRegistry.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#include <Windows.h>

namespace RC::UNBSE
{
    namespace
    {
        std::atomic<FAddonRegistry*> PublishedRegistry{};

        constexpr std::uint32_t KnownEffects =
                UNBSE_ADDON_EFFECT_RUNTIME_READ |
                UNBSE_ADDON_EFFECT_RUNTIME_WRITE |
                UNBSE_ADDON_EFFECT_FILE_IO |
                UNBSE_ADDON_EFFECT_NETWORK_IO;

        template <std::size_t Size>
        auto CopyBounded(char (&Target)[Size], const std::string& Value) -> void
        {
            static_assert(Size > 0);
            const auto Bytes = std::min(Value.size(), Size - 1);
            std::memset(Target, 0, Size);
            std::memcpy(Target, Value.data(), Bytes);
        }

        auto IsCanonicalId(const char* Value) -> bool
        {
            if (!Value || !std::memchr(Value, '\0', UNBSE_ADDON_ID_BYTES))
            {
                return false;
            }
            const auto Length = std::strlen(Value);
            if (Length == 0 || Length >= UNBSE_ADDON_ID_BYTES || Value[0] < 'a' || Value[0] > 'z')
            {
                return false;
            }
            bool PreviousSeparator{};
            for (std::size_t Index = 0; Index < Length; ++Index)
            {
                const auto Character = Value[Index];
                const auto IsAlpha = Character >= 'a' && Character <= 'z';
                const auto IsDigit = Character >= '0' && Character <= '9';
                const auto IsSeparator = Character == '.' || Character == '-';
                if (!IsAlpha && !IsDigit && !IsSeparator)
                {
                    return false;
                }
                if (IsSeparator && (PreviousSeparator || Index + 1 == Length))
                {
                    return false;
                }
                PreviousSeparator = IsSeparator;
            }
            return true;
        }

        auto IsCanonicalVersion(const char* Value) -> bool
        {
            if (!Value || !std::memchr(Value, '\0', UNBSE_ADDON_VERSION_BYTES))
            {
                return false;
            }
            const auto Length = std::strlen(Value);
            if (Length == 0 || Length >= UNBSE_ADDON_VERSION_BYTES || Value[0] < '0' || Value[0] > '9')
            {
                return false;
            }
            for (std::size_t Index = 0; Index < Length; ++Index)
            {
                const auto Character = Value[Index];
                const auto Allowed =
                        (Character >= 'a' && Character <= 'z') ||
                        (Character >= 'A' && Character <= 'Z') ||
                        (Character >= '0' && Character <= '9') ||
                        Character == '.' || Character == '-' || Character == '+';
                if (!Allowed)
                {
                    return false;
                }
            }
            return true;
        }

        auto GetCapabilitiesThunk() -> std::uint32_t
        {
            const auto Registry = PublishedRegistry.load(std::memory_order_acquire);
            return Registry ? Registry->Capabilities() : 0;
        }

        auto QueryRuntimeInfoThunk(UNBSERuntimeInfoV1* Info) -> bool
        {
            const auto Registry = PublishedRegistry.load(std::memory_order_acquire);
            return Registry && Registry->RuntimeInfo(Info);
        }

        auto ResolveRelocationThunk(const UNBSERelocationRequestV1* Request,
                                    UNBSERelocationResultV1* Result) -> std::uint32_t
        {
            const auto Registry = PublishedRegistry.load(std::memory_order_acquire);
            return Registry ? Registry->ResolveExecutableRva(Request, Result)
                            : UNBSE_RELOCATION_RESULT_SHUTTING_DOWN;
        }

        auto RelocationResultNameThunk(const std::uint32_t ResultCode) -> const char*
        {
            switch (ResultCode)
            {
            case UNBSE_RELOCATION_RESULT_OK: return "ok";
            case UNBSE_RELOCATION_RESULT_UNSUPPORTED_HOST_VERSION:
                return "unsupported_host_version";
            case UNBSE_RELOCATION_RESULT_MALFORMED_REQUEST: return "malformed_request";
            case UNBSE_RELOCATION_RESULT_UNKNOWN_OWNER: return "unknown_owner";
            case UNBSE_RELOCATION_RESULT_OWNER_RETIRING: return "owner_retiring";
            case UNBSE_RELOCATION_RESULT_UNSUPPORTED_MODULE: return "unsupported_module";
            case UNBSE_RELOCATION_RESULT_OUT_OF_RANGE: return "out_of_range";
            case UNBSE_RELOCATION_RESULT_SHUTTING_DOWN: return "shutting_down";
            case UNBSE_RELOCATION_RESULT_RUNTIME_UNAVAILABLE: return "runtime_unavailable";
            default: return "unknown_result";
            }
        }

        auto RegisterAddonThunk(const UNBSEAddonDescriptorV1* Descriptor,
                                UNBSEAddonRegistrationV1* Registration) -> std::uint32_t
        {
            const auto Registry = PublishedRegistry.load(std::memory_order_acquire);
            return Registry ? Registry->Register(Descriptor, Registration)
                            : UNBSE_ADDON_RESULT_SHUTTING_DOWN;
        }

        auto RetireAddonThunk(const std::uint32_t OwnerHandle,
                              const std::uint32_t DeadlineMs) -> std::uint32_t
        {
            const auto Registry = PublishedRegistry.load(std::memory_order_acquire);
            return Registry ? Registry->Retire(OwnerHandle, std::chrono::milliseconds{DeadlineMs})
                            : UNBSE_ADDON_RESULT_SHUTTING_DOWN;
        }

        auto ResultNameThunk(const std::uint32_t ResultCode) -> const char*
        {
            switch (ResultCode)
            {
            case UNBSE_ADDON_RESULT_OK: return "ok";
            case UNBSE_ADDON_RESULT_UNSUPPORTED_HOST_VERSION: return "unsupported_host_version";
            case UNBSE_ADDON_RESULT_MALFORMED_DESCRIPTOR: return "malformed_descriptor";
            case UNBSE_ADDON_RESULT_DUPLICATE_ID: return "duplicate_id";
            case UNBSE_ADDON_RESULT_REGISTRY_FULL: return "registry_full";
            case UNBSE_ADDON_RESULT_UNKNOWN_OWNER: return "unknown_owner";
            case UNBSE_ADDON_RESULT_OWNER_RETIRING: return "owner_retiring";
            case UNBSE_ADDON_RESULT_RETIRE_TIMEOUT: return "retire_timeout";
            case UNBSE_ADDON_RESULT_SHUTTING_DOWN: return "shutting_down";
            case UNBSE_ADDON_RESULT_SCRIPT_SERVICE_FAILURE: return "script_service_failure";
            default: return "unknown_result";
            }
        }

        auto MessagingResultNameThunk(const std::uint32_t ResultCode) -> const char*
        {
            switch (ResultCode)
            {
            case UNBSE_MESSAGING_RESULT_OK: return "ok";
            case UNBSE_MESSAGING_RESULT_UNSUPPORTED_HOST_VERSION: return "unsupported_host_version";
            case UNBSE_MESSAGING_RESULT_MALFORMED_REQUEST: return "malformed_request";
            case UNBSE_MESSAGING_RESULT_UNKNOWN_OWNER: return "unknown_owner";
            case UNBSE_MESSAGING_RESULT_OWNER_RETIRING: return "owner_retiring";
            case UNBSE_MESSAGING_RESULT_DUPLICATE_LISTENER: return "duplicate_listener";
            case UNBSE_MESSAGING_RESULT_LISTENER_REGISTRY_FULL: return "listener_registry_full";
            case UNBSE_MESSAGING_RESULT_NO_LISTENERS: return "no_listeners";
            case UNBSE_MESSAGING_RESULT_SHUTTING_DOWN: return "shutting_down";
            default: return "unknown_result";
            }
        }

        auto RegisterListenerThunk(const std::uint32_t OwnerHandle,
                                   const char* SenderFilter,
                                   const UNBSEMessageCallbackV1 Callback,
                                   void* Context) -> std::uint32_t
        {
            const auto Registry = PublishedRegistry.load(std::memory_order_acquire);
            return Registry ? Registry->RegisterListener(OwnerHandle, SenderFilter, Callback, Context)
                            : UNBSE_MESSAGING_RESULT_SHUTTING_DOWN;
        }

        auto DispatchMessageThunk(const std::uint32_t SenderOwnerHandle,
                                  const std::uint32_t MessageType,
                                  const void* Data,
                                  const std::uint32_t DataSize,
                                  const char* ReceiverId) -> std::uint32_t
        {
            const auto Registry = PublishedRegistry.load(std::memory_order_acquire);
            return Registry ? Registry->Dispatch(SenderOwnerHandle, MessageType, Data, DataSize,
                                                 ReceiverId)
                            : UNBSE_MESSAGING_RESULT_SHUTTING_DOWN;
        }

        auto RegistrationEvent(const char* AddonId,
                               const char* AddonVersion,
                               const std::uint32_t DeclaredEffects,
                               const std::uint32_t RequiredHostCapabilities,
                               const std::uint32_t HostCapabilities,
                               const std::uint32_t MissingHostCapabilities,
                               const std::uint32_t Compatibility,
                               const std::uint32_t ResultCode) -> std::string
        {
            const auto Status = ResultCode != UNBSE_ADDON_RESULT_OK
                                        ? "rejected"
                                        : Compatibility == UNBSE_ADDON_COMPATIBILITY_VERIFIED
                                                  ? "verified"
                                                  : "unverified-attempt";
            std::ostringstream Json{};
            Json << "{\"schema\":\"UNBSE.AddonCompatibility\",\"schemaVersion\":1,"
                    "\"event\":\"registration\",\"addonId\":\""
                 << (AddonId ? AddonId : "") << "\",\"version\":\""
                 << (AddonVersion ? AddonVersion : "") << "\",\"status\":\"" << Status
                 << "\",\"policy\":\"report-and-attempt\",\"resultCode\":" << ResultCode
                 << ",\"result\":\"" << ResultNameThunk(ResultCode)
                 << "\",\"declaredEffects\":" << DeclaredEffects
                 << ",\"requiredHostCapabilities\":" << RequiredHostCapabilities
                 << ",\"hostCapabilities\":" << HostCapabilities
                 << ",\"missingHostCapabilities\":" << MissingHostCapabilities << '}';
            return Json.str();
        }

        auto RetirementEvent(const std::uint32_t OwnerHandle,
                             const char* AddonId,
                             const std::uint32_t ResultCode) -> std::string
        {
            std::ostringstream Json{};
            Json << "{\"schema\":\"UNBSE.AddonCompatibility\",\"schemaVersion\":1,"
                    "\"event\":\"retirement\",\"addonId\":\""
                 << (AddonId ? AddonId : "") << "\",\"ownerHandle\":" << OwnerHandle
                 << ",\"status\":\""
                 << (ResultCode == UNBSE_ADDON_RESULT_OK ? "retired" : "retirement-failed")
                 << "\",\"resultCode\":" << ResultCode << ",\"result\":\""
                 << ResultNameThunk(ResultCode) << "\"}";
            return Json.str();
        }
    } // namespace

    FAddonRegistry::FAddonRegistry(FScriptServiceRegistry& ScriptRegistry,
                                   FAddonRegistryEventSink EventSink,
                                   FAddonRuntimeIdentity RuntimeIdentity)
        : m_script_registry(ScriptRegistry),
          m_event_sink(std::move(EventSink)),
          m_runtime_identity(std::move(RuntimeIdentity))
    {
    }

    FAddonRegistry::~FAddonRegistry()
    {
        BeginShutdown(std::chrono::milliseconds{0});
    }

    auto FAddonRegistry::Register(const UNBSEAddonDescriptorV1* Descriptor,
                                  UNBSEAddonRegistrationV1* Registration) -> std::uint32_t
    {
        if (!Descriptor || !Registration || Descriptor->structSize != sizeof(UNBSEAddonDescriptorV1) ||
            Descriptor->apiVersion != UNBSE_ADDON_HOST_ABI_VERSION ||
            Registration->structSize != sizeof(UNBSEAddonRegistrationV1) ||
            Registration->apiVersion != UNBSE_ADDON_HOST_ABI_VERSION ||
            Descriptor->declaredEffects == 0 ||
            (Descriptor->declaredEffects & ~KnownEffects) != 0 ||
            !IsCanonicalId(Descriptor->addonId) || !IsCanonicalVersion(Descriptor->addonVersion) ||
            std::any_of(std::begin(Descriptor->reserved), std::end(Descriptor->reserved),
                        [](const std::uint64_t Value) { return Value != 0; }) ||
            std::any_of(std::begin(Registration->reserved), std::end(Registration->reserved),
                        [](const std::uint64_t Value) { return Value != 0; }))
        {
            if (m_event_sink)
            {
                m_event_sink(RegistrationEvent(nullptr, nullptr, 0, 0, Capabilities(), 0,
                                               UNBSE_ADDON_COMPATIBILITY_UNVERIFIED_ATTEMPT,
                                               UNBSE_ADDON_RESULT_MALFORMED_DESCRIPTOR));
            }
            return UNBSE_ADDON_RESULT_MALFORMED_DESCRIPTOR;
        }
        const auto HostCapabilities = Capabilities();
        const auto MissingHostCapabilities =
                Descriptor->requiredHostCapabilities & ~HostCapabilities;

        std::lock_guard<std::mutex> Lock(m_mutex);
        if (m_shutting_down)
        {
            if (m_event_sink)
            {
                m_event_sink(RegistrationEvent(
                        Descriptor->addonId, Descriptor->addonVersion, Descriptor->declaredEffects,
                        Descriptor->requiredHostCapabilities, HostCapabilities,
                        MissingHostCapabilities, UNBSE_ADDON_COMPATIBILITY_UNVERIFIED_ATTEMPT,
                        UNBSE_ADDON_RESULT_SHUTTING_DOWN));
            }
            return UNBSE_ADDON_RESULT_SHUTTING_DOWN;
        }
        for (const auto& Entry : m_entries)
        {
            if (Entry.Occupied && Entry.AddonId == Descriptor->addonId)
            {
                if (m_event_sink)
                {
                    m_event_sink(RegistrationEvent(
                            Descriptor->addonId, Descriptor->addonVersion,
                            Descriptor->declaredEffects, Descriptor->requiredHostCapabilities,
                            HostCapabilities, MissingHostCapabilities,
                            UNBSE_ADDON_COMPATIBILITY_UNVERIFIED_ATTEMPT,
                            UNBSE_ADDON_RESULT_DUPLICATE_ID));
                }
                return UNBSE_ADDON_RESULT_DUPLICATE_ID;
            }
        }
        const auto Free = std::find_if(m_entries.begin(), m_entries.end(),
                                       [](const FEntry& Entry) { return !Entry.Occupied; });
        if (Free == m_entries.end())
        {
            if (m_event_sink)
            {
                m_event_sink(RegistrationEvent(
                        Descriptor->addonId, Descriptor->addonVersion, Descriptor->declaredEffects,
                        Descriptor->requiredHostCapabilities, HostCapabilities,
                        MissingHostCapabilities, UNBSE_ADDON_COMPATIBILITY_UNVERIFIED_ATTEMPT,
                        UNBSE_ADDON_RESULT_REGISTRY_FULL));
            }
            return UNBSE_ADDON_RESULT_REGISTRY_FULL;
        }
        if (m_next_owner_handle == 0 || m_next_owner_handle == FScriptServiceRegistry::CoreOwnerHandle)
        {
            if (m_event_sink)
            {
                m_event_sink(RegistrationEvent(
                        Descriptor->addonId, Descriptor->addonVersion, Descriptor->declaredEffects,
                        Descriptor->requiredHostCapabilities, HostCapabilities,
                        MissingHostCapabilities, UNBSE_ADDON_COMPATIBILITY_UNVERIFIED_ATTEMPT,
                        UNBSE_ADDON_RESULT_REGISTRY_FULL));
            }
            return UNBSE_ADDON_RESULT_REGISTRY_FULL;
        }

        auto& Entry = *Free;
        Entry.Occupied = true;
        Entry.OwnerHandle = m_next_owner_handle++;
        Entry.DeclaredEffects = Descriptor->declaredEffects;
        Entry.AddonId = Descriptor->addonId;
        Entry.AddonVersion = Descriptor->addonVersion;
        ++m_addon_count;

        *Registration = {
                sizeof(UNBSEAddonRegistrationV1),
                UNBSE_ADDON_HOST_ABI_VERSION,
                Entry.OwnerHandle,
                HostCapabilities,
                MissingHostCapabilities,
                static_cast<std::uint32_t>(
                        MissingHostCapabilities == 0
                                ? UNBSE_ADDON_COMPATIBILITY_VERIFIED
                                : UNBSE_ADDON_COMPATIBILITY_UNVERIFIED_ATTEMPT),
                {},
        };
        if (m_event_sink)
        {
            m_event_sink(RegistrationEvent(
                    Descriptor->addonId, Descriptor->addonVersion, Descriptor->declaredEffects,
                    Descriptor->requiredHostCapabilities, HostCapabilities,
                    MissingHostCapabilities, Registration->compatibility, UNBSE_ADDON_RESULT_OK));
        }
        return UNBSE_ADDON_RESULT_OK;
    }

    auto FAddonRegistry::Retire(const std::uint32_t OwnerHandle,
                                const std::chrono::milliseconds Timeout) -> std::uint32_t
    {
        std::string AddonId{};
        {
            std::unique_lock<std::mutex> Lock(m_mutex);
            const auto Entry = std::find_if(m_entries.begin(), m_entries.end(),
                                            [OwnerHandle](const FEntry& Candidate) {
                                                return Candidate.Occupied &&
                                                       Candidate.OwnerHandle == OwnerHandle;
                                            });
            if (Entry == m_entries.end())
            {
                if (m_event_sink)
                {
                    m_event_sink(RetirementEvent(OwnerHandle, nullptr,
                                                 UNBSE_ADDON_RESULT_UNKNOWN_OWNER));
                }
                return UNBSE_ADDON_RESULT_UNKNOWN_OWNER;
            }
            AddonId = Entry->AddonId;
            if (Entry->Retiring)
            {
                if (m_event_sink)
                {
                    m_event_sink(RetirementEvent(OwnerHandle, AddonId.c_str(),
                                                 UNBSE_ADDON_RESULT_OWNER_RETIRING));
                }
                return UNBSE_ADDON_RESULT_OWNER_RETIRING;
            }
            Entry->Retiring = true;

            const auto Deadline = std::chrono::steady_clock::now() +
                                  std::max(Timeout, std::chrono::milliseconds::zero());
            while (OwnerMessageCallsLocked(OwnerHandle) != 0)
            {
                if (m_changed.wait_until(Lock, Deadline) == std::cv_status::timeout)
                {
                    Entry->Retiring = false;
                    if (m_event_sink)
                    {
                        m_event_sink(RetirementEvent(OwnerHandle, AddonId.c_str(),
                                                     UNBSE_ADDON_RESULT_RETIRE_TIMEOUT));
                    }
                    return UNBSE_ADDON_RESULT_RETIRE_TIMEOUT;
                }
            }
            RemoveOwnerListenersLocked(OwnerHandle);
        }

        const auto ScriptResult = m_script_registry.RetireOwner(OwnerHandle, Timeout);
        if (ScriptResult != UNBSE_SCRIPT_RESULT_OK)
        {
            std::lock_guard<std::mutex> Lock(m_mutex);
            const auto Entry = std::find_if(m_entries.begin(), m_entries.end(),
                                            [OwnerHandle](const FEntry& Candidate) {
                                                return Candidate.Occupied &&
                                                       Candidate.OwnerHandle == OwnerHandle;
                                            });
            if (Entry != m_entries.end())
            {
                Entry->Retiring = false;
            }
            const auto Result = ScriptResult == UNBSE_SCRIPT_RESULT_RETIRE_TIMEOUT
                                        ? UNBSE_ADDON_RESULT_RETIRE_TIMEOUT
                                        : UNBSE_ADDON_RESULT_SCRIPT_SERVICE_FAILURE;
            if (m_event_sink)
            {
                m_event_sink(RetirementEvent(OwnerHandle, AddonId.c_str(), Result));
            }
            return Result;
        }

        std::lock_guard<std::mutex> Lock(m_mutex);
        const auto Entry = std::find_if(m_entries.begin(), m_entries.end(),
                                        [OwnerHandle](const FEntry& Candidate) {
                                            return Candidate.Occupied &&
                                                   Candidate.OwnerHandle == OwnerHandle;
                                        });
        if (Entry != m_entries.end())
        {
            *Entry = {};
            --m_addon_count;
        }
        if (m_event_sink)
        {
            m_event_sink(RetirementEvent(OwnerHandle, AddonId.c_str(), UNBSE_ADDON_RESULT_OK));
        }
        return UNBSE_ADDON_RESULT_OK;
    }

    auto FAddonRegistry::FindOwnerLocked(const std::uint32_t OwnerHandle) -> FEntry*
    {
        const auto Entry = std::find_if(m_entries.begin(), m_entries.end(),
                                        [OwnerHandle](const FEntry& Candidate) {
                                            return Candidate.Occupied &&
                                                   Candidate.OwnerHandle == OwnerHandle;
                                        });
        return Entry == m_entries.end() ? nullptr : &*Entry;
    }

    auto FAddonRegistry::OwnerMessageCallsLocked(const std::uint32_t OwnerHandle) const
            -> std::size_t
    {
        std::size_t Active{};
        for (const auto& Listener : m_listeners)
        {
            if (Listener.Occupied && Listener.OwnerHandle == OwnerHandle)
            {
                Active += Listener.ActiveCalls;
            }
        }
        return Active;
    }

    auto FAddonRegistry::RemoveOwnerListenersLocked(const std::uint32_t OwnerHandle) -> void
    {
        for (auto& Listener : m_listeners)
        {
            if (Listener.Occupied && Listener.OwnerHandle == OwnerHandle)
            {
                Listener = {};
            }
        }
    }

    auto FAddonRegistry::RegisterListener(const std::uint32_t OwnerHandle,
                                          const char* SenderFilter,
                                          const UNBSEMessageCallbackV1 Callback,
                                          void* Context) -> std::uint32_t
    {
        if (!Callback || (SenderFilter && !IsCanonicalId(SenderFilter)))
        {
            return UNBSE_MESSAGING_RESULT_MALFORMED_REQUEST;
        }
        const std::string Filter = SenderFilter ? SenderFilter : "";
        std::lock_guard<std::mutex> Lock(m_mutex);
        if (m_shutting_down)
        {
            return UNBSE_MESSAGING_RESULT_SHUTTING_DOWN;
        }
        const auto Owner = FindOwnerLocked(OwnerHandle);
        if (!Owner)
        {
            return UNBSE_MESSAGING_RESULT_UNKNOWN_OWNER;
        }
        if (Owner->Retiring)
        {
            return UNBSE_MESSAGING_RESULT_OWNER_RETIRING;
        }
        for (const auto& Listener : m_listeners)
        {
            if (Listener.Occupied && Listener.OwnerHandle == OwnerHandle &&
                Listener.SenderFilter == Filter && Listener.Callback == Callback &&
                Listener.Context == Context)
            {
                return UNBSE_MESSAGING_RESULT_DUPLICATE_LISTENER;
            }
        }
        const auto Free = std::find_if(m_listeners.begin(), m_listeners.end(),
                                       [](const FListener& Listener) {
                                           return !Listener.Occupied;
                                       });
        if (Free == m_listeners.end())
        {
            return UNBSE_MESSAGING_RESULT_LISTENER_REGISTRY_FULL;
        }
        *Free = {true, OwnerHandle, Filter, Callback, Context, 0};
        return UNBSE_MESSAGING_RESULT_OK;
    }

    auto FAddonRegistry::Dispatch(const std::uint32_t SenderOwnerHandle,
                                  const std::uint32_t MessageType,
                                  const void* Data,
                                  const std::uint32_t DataSize,
                                  const char* ReceiverId) -> std::uint32_t
    {
        if (MessageType == 0 || DataSize > UNBSE_MESSAGE_MAX_BYTES ||
            (DataSize != 0 && !Data) || (ReceiverId && !IsCanonicalId(ReceiverId)))
        {
            return UNBSE_MESSAGING_RESULT_MALFORMED_REQUEST;
        }

        struct FTarget
        {
            std::size_t Index{};
            UNBSEMessageCallbackV1 Callback{};
            void* Context{};
        };
        std::vector<FTarget> Targets{};
        std::string SenderId{};
        {
            std::lock_guard<std::mutex> Lock(m_mutex);
            if (m_shutting_down)
            {
                return UNBSE_MESSAGING_RESULT_SHUTTING_DOWN;
            }
            if (SenderOwnerHandle == FScriptServiceRegistry::CoreOwnerHandle)
            {
                SenderId = "unbse.core";
            }
            else
            {
                const auto Sender = FindOwnerLocked(SenderOwnerHandle);
                if (!Sender)
                {
                    return UNBSE_MESSAGING_RESULT_UNKNOWN_OWNER;
                }
                if (Sender->Retiring)
                {
                    return UNBSE_MESSAGING_RESULT_OWNER_RETIRING;
                }
                SenderId = Sender->AddonId;
            }

            for (std::size_t Index = 0; Index < m_listeners.size(); ++Index)
            {
                auto& Listener = m_listeners[Index];
                if (!Listener.Occupied ||
                    (!Listener.SenderFilter.empty() && Listener.SenderFilter != SenderId))
                {
                    continue;
                }
                const auto Owner = FindOwnerLocked(Listener.OwnerHandle);
                if (!Owner || Owner->Retiring ||
                    (ReceiverId && Owner->AddonId != ReceiverId))
                {
                    continue;
                }
                ++Listener.ActiveCalls;
                Targets.push_back({Index, Listener.Callback, Listener.Context});
            }
        }

        if (Targets.empty())
        {
            return UNBSE_MESSAGING_RESULT_NO_LISTENERS;
        }

        UNBSEMessageV1 Message{};
        Message.structSize = sizeof(Message);
        Message.messageType = MessageType;
        Message.dataSize = DataSize;
        std::memcpy(Message.senderId, SenderId.c_str(), SenderId.size() + 1);
        Message.data = Data;
        for (const auto& Target : Targets)
        {
            Target.Callback(&Message, Target.Context);
            std::lock_guard<std::mutex> Lock(m_mutex);
            auto& Listener = m_listeners[Target.Index];
            if (Listener.Occupied && Listener.ActiveCalls != 0)
            {
                --Listener.ActiveCalls;
                m_changed.notify_all();
            }
        }
        return UNBSE_MESSAGING_RESULT_OK;
    }

    auto FAddonRegistry::DispatchCore(const std::uint32_t MessageType,
                                      const void* Data,
                                      const std::uint32_t DataSize) -> std::uint32_t
    {
        return Dispatch(FScriptServiceRegistry::CoreOwnerHandle, MessageType, Data, DataSize,
                        nullptr);
    }

    auto FAddonRegistry::BeginShutdown(const std::chrono::milliseconds Timeout) -> std::uint32_t
    {
        std::vector<std::uint32_t> Owners{};
        {
            std::lock_guard<std::mutex> Lock(m_mutex);
            if (m_shutting_down && m_addon_count == 0)
            {
                return UNBSE_ADDON_RESULT_OK;
            }
            m_shutting_down = true;
            Owners.reserve(m_addon_count);
            for (const auto& Entry : m_entries)
            {
                if (Entry.Occupied)
                {
                    Owners.push_back(Entry.OwnerHandle);
                }
            }
        }

        const auto Deadline = std::chrono::steady_clock::now() +
                              std::max(Timeout, std::chrono::milliseconds::zero());
        for (const auto Owner : Owners)
        {
            const auto Now = std::chrono::steady_clock::now();
            const auto Remaining = Now < Deadline
                                           ? std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - Now)
                                           : std::chrono::milliseconds::zero();
            const auto Result = Retire(Owner, Remaining);
            if (Result != UNBSE_ADDON_RESULT_OK && Result != UNBSE_ADDON_RESULT_OWNER_RETIRING)
            {
                return Result;
            }
        }
        return UNBSE_ADDON_RESULT_OK;
    }

    auto FAddonRegistry::Capabilities() const -> std::uint32_t
    {
        return UNBSE_ADDON_HOST_CAPABILITY_SCRIPT_SERVICE_V1 |
               UNBSE_ADDON_HOST_CAPABILITY_MESSAGING_V1 |
               UNBSE_ADDON_HOST_CAPABILITY_RUNTIME_INFO_V1 |
               UNBSE_ADDON_HOST_CAPABILITY_RELOCATION_V1;
    }

    auto FAddonRegistry::RuntimeInfo(UNBSERuntimeInfoV1* Info) const -> bool
    {
        if (!Info || Info->structSize != sizeof(UNBSERuntimeInfoV1) ||
            Info->apiVersion != UNBSE_RUNTIME_INFO_ABI_VERSION)
        {
            return false;
        }

        UNBSERuntimeInfoV1 Observed{};
        Observed.structSize = sizeof(Observed);
        Observed.apiVersion = UNBSE_RUNTIME_INFO_ABI_VERSION;
        Observed.platform = UNBSE_RUNTIME_PLATFORM_WINDOWS_X64;
        Observed.processId = GetCurrentProcessId();
        Observed.identityFlags = UNBSE_RUNTIME_IDENTITY_PROCESS_OBSERVED;
        CopyBounded(Observed.hostVersion, m_runtime_identity.HostVersion);
        CopyBounded(Observed.foundationId, m_runtime_identity.FoundationId);
        if (!m_runtime_identity.FoundationId.empty())
        {
            Observed.identityFlags |= UNBSE_RUNTIME_IDENTITY_FOUNDATION_DECLARED;
        }

        const auto Module = GetModuleHandleW(nullptr);
        if (Module)
        {
            const auto BaseAddress = reinterpret_cast<std::uintptr_t>(Module);
            Observed.imageBase = static_cast<std::uint64_t>(BaseAddress);

            std::array<char, 32768> ModulePath{};
            const auto PathLength = GetModuleFileNameA(
                    Module, ModulePath.data(), static_cast<DWORD>(ModulePath.size()));
            if (PathLength > 0 && PathLength < ModulePath.size())
            {
                const std::string FullPath{ModulePath.data(), PathLength};
                const auto Separator = FullPath.find_last_of("\\/");
                CopyBounded(Observed.executableName,
                            Separator == std::string::npos ? FullPath : FullPath.substr(Separator + 1));
            }

            const auto Base = reinterpret_cast<const std::uint8_t*>(Module);
            const auto Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
            if (Dos->e_magic == IMAGE_DOS_SIGNATURE && Dos->e_lfanew > 0 &&
                Dos->e_lfanew <= 1024 * 1024)
            {
                const auto Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
                if (Nt->Signature == IMAGE_NT_SIGNATURE &&
                    Nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                    Nt->OptionalHeader.SizeOfImage > 0)
                {
                    Observed.identityFlags |= UNBSE_RUNTIME_IDENTITY_PE_OBSERVED;
                    Observed.peTimestamp = Nt->FileHeader.TimeDateStamp;
                    Observed.peChecksum = Nt->OptionalHeader.CheckSum;
                    Observed.peMachine = Nt->FileHeader.Machine;
                    Observed.imageSize = Nt->OptionalHeader.SizeOfImage;
                }
            }
        }

        *Info = Observed;
        return true;
    }

    auto FAddonRegistry::ResolveExecutableRva(const UNBSERelocationRequestV1* Request,
                                              UNBSERelocationResultV1* Result) -> std::uint32_t
    {
        if (!Request || !Result || Request->structSize != sizeof(UNBSERelocationRequestV1) ||
            Request->apiVersion != UNBSE_RELOCATION_ABI_VERSION ||
            Result->structSize != sizeof(UNBSERelocationResultV1) ||
            Result->apiVersion != UNBSE_RELOCATION_ABI_VERSION || Request->minimumBytes == 0 ||
            std::any_of(std::begin(Request->reserved), std::end(Request->reserved),
                        [](const std::uint64_t Value) { return Value != 0; }) ||
            std::any_of(std::begin(Result->reserved), std::end(Result->reserved),
                        [](const std::uint64_t Value) { return Value != 0; }))
        {
            return UNBSE_RELOCATION_RESULT_MALFORMED_REQUEST;
        }
        if (Request->module != UNBSE_RELOCATION_MODULE_EXECUTABLE)
        {
            return UNBSE_RELOCATION_RESULT_UNSUPPORTED_MODULE;
        }

        {
            std::lock_guard<std::mutex> Lock(m_mutex);
            if (m_shutting_down)
            {
                return UNBSE_RELOCATION_RESULT_SHUTTING_DOWN;
            }
            const auto Owner = FindOwnerLocked(Request->ownerHandle);
            if (!Owner)
            {
                return UNBSE_RELOCATION_RESULT_UNKNOWN_OWNER;
            }
            if (Owner->Retiring)
            {
                return UNBSE_RELOCATION_RESULT_OWNER_RETIRING;
            }
        }

        UNBSERuntimeInfoV1 Info{};
        Info.structSize = sizeof(Info);
        Info.apiVersion = UNBSE_RUNTIME_INFO_ABI_VERSION;
        if (!RuntimeInfo(&Info) ||
            (Info.identityFlags & UNBSE_RUNTIME_IDENTITY_PE_OBSERVED) == 0 ||
            Info.imageBase == 0 || Info.imageSize == 0)
        {
            return UNBSE_RELOCATION_RESULT_RUNTIME_UNAVAILABLE;
        }
        if (Request->relativeAddress >= Info.imageSize ||
            Request->minimumBytes > Info.imageSize - Request->relativeAddress ||
            Request->relativeAddress >
                    std::numeric_limits<std::uint64_t>::max() - Info.imageBase)
        {
            return UNBSE_RELOCATION_RESULT_OUT_OF_RANGE;
        }

        UNBSERelocationResultV1 Resolved{};
        Resolved.structSize = sizeof(Resolved);
        Resolved.apiVersion = UNBSE_RELOCATION_ABI_VERSION;
        Resolved.absoluteAddress = Info.imageBase + Request->relativeAddress;
        Resolved.imageBase = Info.imageBase;
        Resolved.imageSize = Info.imageSize;
        Resolved.relativeAddress = Request->relativeAddress;
        *Result = Resolved;
        return UNBSE_RELOCATION_RESULT_OK;
    }

    auto FAddonRegistry::AddonCount() const -> std::size_t
    {
        std::lock_guard<std::mutex> Lock(m_mutex);
        return m_addon_count;
    }

    auto PublishAddonRegistry(FAddonRegistry* Registry) -> bool
    {
        if (!Registry)
        {
            return false;
        }
        FAddonRegistry* Expected{};
        return PublishedRegistry.compare_exchange_strong(Expected, Registry, std::memory_order_acq_rel);
    }

    auto UnpublishAddonRegistry(FAddonRegistry* Registry) -> void
    {
        PublishedRegistry.compare_exchange_strong(Registry, nullptr, std::memory_order_acq_rel);
    }

    auto GetPublishedAddonRegistry() -> FAddonRegistry*
    {
        return PublishedRegistry.load(std::memory_order_acquire);
    }
} // namespace RC::UNBSE

extern "C" UNBSE_SCRIPT_EXPORT int32_t UNBSE_SCRIPT_CALL UNBSE_QueryAddonHostV1(
        const uint32_t RequestedVersion,
        UNBSEAddonHostV1* Host)
{
    if (!Host || RequestedVersion != UNBSE_ADDON_HOST_ABI_VERSION ||
        Host->structSize != sizeof(UNBSEAddonHostV1))
    {
        return 0;
    }
    *Host = {
            sizeof(UNBSEAddonHostV1),
            UNBSE_ADDON_HOST_ABI_VERSION,
            &RC::UNBSE::GetCapabilitiesThunk,
            &RC::UNBSE::RegisterAddonThunk,
            &RC::UNBSE::RetireAddonThunk,
            &RC::UNBSE::ResultNameThunk,
            &UNBSE_QueryScriptServiceV1,
            {},
    };
    return 1;
}

extern "C" UNBSE_SCRIPT_EXPORT int32_t UNBSE_SCRIPT_CALL UNBSE_QueryMessagingV1(
        const uint32_t RequestedVersion,
        UNBSEMessagingV1* Messaging)
{
    if (!Messaging || RequestedVersion != UNBSE_MESSAGING_ABI_VERSION ||
        Messaging->structSize != sizeof(UNBSEMessagingV1))
    {
        return 0;
    }
    *Messaging = {
            sizeof(UNBSEMessagingV1),
            UNBSE_MESSAGING_ABI_VERSION,
            &RC::UNBSE::RegisterListenerThunk,
            &RC::UNBSE::DispatchMessageThunk,
            &RC::UNBSE::MessagingResultNameThunk,
            {},
    };
    return 1;
}

extern "C" UNBSE_SCRIPT_EXPORT int32_t UNBSE_SCRIPT_CALL UNBSE_QueryRuntimeInfoV1(
        const uint32_t RequestedVersion,
        UNBSERuntimeInfoV1* RuntimeInfo)
{
    if (!RuntimeInfo || RequestedVersion != UNBSE_RUNTIME_INFO_ABI_VERSION ||
        RuntimeInfo->structSize != sizeof(UNBSERuntimeInfoV1) ||
        RuntimeInfo->apiVersion != UNBSE_RUNTIME_INFO_ABI_VERSION)
    {
        return 0;
    }
    return RC::UNBSE::QueryRuntimeInfoThunk(RuntimeInfo) ? 1 : 0;
}

extern "C" UNBSE_SCRIPT_EXPORT int32_t UNBSE_SCRIPT_CALL UNBSE_QueryRelocationV1(
        const uint32_t RequestedVersion,
        UNBSERelocationV1* Relocation)
{
    if (!Relocation || RequestedVersion != UNBSE_RELOCATION_ABI_VERSION ||
        Relocation->structSize != sizeof(UNBSERelocationV1))
    {
        return 0;
    }
    *Relocation = {
            sizeof(UNBSERelocationV1),
            UNBSE_RELOCATION_ABI_VERSION,
            &RC::UNBSE::ResolveRelocationThunk,
            &RC::UNBSE::RelocationResultNameThunk,
            {},
    };
    return 1;
}
