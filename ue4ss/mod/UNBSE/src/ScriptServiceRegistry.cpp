#include <ScriptServiceRegistry.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>

namespace RC::UNBSE
{
    namespace
    {
        std::atomic<FScriptServiceRegistry*> PublishedRegistry{};

        auto IsCanonicalToken(const char* Value) -> bool
        {
            if (!Value || Value[0] < 'a' || Value[0] > 'z')
            {
                return false;
            }
            for (std::size_t Index = 1; Index < UNBSE_SCRIPT_TOKEN_BYTES; ++Index)
            {
                const auto Character = static_cast<unsigned char>(Value[Index]);
                if (Character == 0)
                {
                    return true;
                }
                if (!((Character >= 'a' && Character <= 'z') ||
                      (Character >= '0' && Character <= '9') || Character == '_'))
                {
                    return false;
                }
            }
            return false;
        }

        auto IsKnownValueType(const std::uint32_t Type, const bool AllowNone) -> bool
        {
            switch (Type)
            {
            case UNBSE_SCRIPT_VALUE_NONE:
                return AllowNone;
            case UNBSE_SCRIPT_VALUE_BOOL:
            case UNBSE_SCRIPT_VALUE_INT64:
            case UNBSE_SCRIPT_VALUE_FLOAT64:
            case UNBSE_SCRIPT_VALUE_UTF8:
                return true;
            default:
                return false;
            }
        }

        auto IsKnownResultCode(const std::uint32_t Code) -> bool
        {
            switch (Code)
            {
            case UNBSE_SCRIPT_RESULT_OK:
            case UNBSE_SCRIPT_RESULT_UNSUPPORTED_SERVICE_VERSION:
            case UNBSE_SCRIPT_RESULT_MALFORMED_DECLARATION:
            case UNBSE_SCRIPT_RESULT_NAMESPACE_VIOLATION:
            case UNBSE_SCRIPT_RESULT_DUPLICATE_NAME:
            case UNBSE_SCRIPT_RESULT_REGISTRY_FULL:
            case UNBSE_SCRIPT_RESULT_OWNER_LIMIT:
            case UNBSE_SCRIPT_RESULT_OWNER_RETIRED:
            case UNBSE_SCRIPT_RESULT_VM_UNAVAILABLE:
            case UNBSE_SCRIPT_RESULT_VM_THREAD_MISMATCH:
            case UNBSE_SCRIPT_RESULT_FUNCTION_NOT_FOUND:
            case UNBSE_SCRIPT_RESULT_ARGUMENT_MISMATCH:
            case UNBSE_SCRIPT_RESULT_CALLBACK_FAILED:
            case UNBSE_SCRIPT_RESULT_DEADLINE_EXPIRED:
            case UNBSE_SCRIPT_RESULT_RETIRE_TIMEOUT:
            case UNBSE_SCRIPT_RESULT_SHUTTING_DOWN:
            case UNBSE_SCRIPT_RESULT_NAMESPACE_COLLISION:
                return true;
            default:
                return false;
            }
        }

        auto IsCanonicalValue(const UNBSEScriptValueV1& Value, const std::uint32_t ExpectedType) -> bool
        {
            if (Value.structSize != sizeof(UNBSEScriptValueV1) || Value.type != ExpectedType || Value.reserved[0] != 0)
            {
                return false;
            }
            if (Value.type == UNBSE_SCRIPT_VALUE_BOOL && Value.integerValue != 0 && Value.integerValue != 1)
            {
                return false;
            }
            if (Value.type == UNBSE_SCRIPT_VALUE_UTF8 &&
                std::memchr(Value.utf8Value, '\0', UNBSE_SCRIPT_UTF8_BYTES) == nullptr)
            {
                return false;
            }
            return true;
        }

        auto MakeFailure(const std::uint32_t Code, std::string Diagnostic) -> FScriptInvocationResult
        {
            FScriptInvocationResult Result{};
            Result.Code = Code;
            Result.Value.structSize = sizeof(UNBSEScriptValueV1);
            Result.Value.type = UNBSE_SCRIPT_VALUE_NONE;
            Result.Diagnostic = std::move(Diagnostic);
            return Result;
        }

        auto GetCapabilitiesThunk() -> std::uint32_t
        {
            const auto Registry = PublishedRegistry.load(std::memory_order_acquire);
            return Registry ? Registry->Capabilities() : 0;
        }

        auto RegisterFunctionThunk(const std::uint32_t OwnerHandle,
                                   const UNBSEScriptFunctionDeclarationV1* Declaration) -> std::uint32_t
        {
            const auto Registry = PublishedRegistry.load(std::memory_order_acquire);
            return Registry ? Registry->Register(OwnerHandle, Declaration) : UNBSE_SCRIPT_RESULT_VM_UNAVAILABLE;
        }

        auto RetireOwnerThunk(const std::uint32_t OwnerHandle, const std::uint32_t DeadlineMs) -> std::uint32_t
        {
            const auto Registry = PublishedRegistry.load(std::memory_order_acquire);
            return Registry ? Registry->RetireOwner(OwnerHandle, std::chrono::milliseconds{DeadlineMs})
                            : UNBSE_SCRIPT_RESULT_VM_UNAVAILABLE;
        }
    } // namespace

    FScriptServiceRegistry::~FScriptServiceRegistry()
    {
        BeginShutdown(std::chrono::milliseconds{0});
    }

    auto FScriptServiceRegistry::Register(
            const std::uint32_t OwnerHandle,
            const UNBSEScriptFunctionDeclarationV1* Declaration) -> std::uint32_t
    {
        if (!Declaration || Declaration->structSize != sizeof(UNBSEScriptFunctionDeclarationV1) ||
            Declaration->apiVersion != UNBSE_SCRIPT_SERVICE_ABI_VERSION || Declaration->reserved != 0 ||
            Declaration->reserved2[0] != 0 || Declaration->reserved2[1] != 0 ||
            Declaration->flags != UNBSE_SCRIPT_FUNCTION_READ_ONLY ||
            Declaration->argumentCount > UNBSE_SCRIPT_MAX_ARGUMENTS || !Declaration->callback ||
            !IsKnownValueType(Declaration->resultType, true) ||
            !IsCanonicalToken(Declaration->namespaceToken) || !IsCanonicalToken(Declaration->functionToken))
        {
            return UNBSE_SCRIPT_RESULT_MALFORMED_DECLARATION;
        }
        if (OwnerHandle == 0)
        {
            return UNBSE_SCRIPT_RESULT_OWNER_RETIRED;
        }
        if (std::strcmp(Declaration->namespaceToken, "core") == 0 && OwnerHandle != CoreOwnerHandle)
        {
            return UNBSE_SCRIPT_RESULT_NAMESPACE_VIOLATION;
        }
        for (std::size_t Index = 0; Index < UNBSE_SCRIPT_MAX_ARGUMENTS; ++Index)
        {
            const auto Expected = Index < Declaration->argumentCount;
            if ((Expected && !IsKnownValueType(Declaration->argumentTypes[Index], false)) ||
                (!Expected && Declaration->argumentTypes[Index] != UNBSE_SCRIPT_VALUE_NONE))
            {
                return UNBSE_SCRIPT_RESULT_MALFORMED_DECLARATION;
            }
        }

        std::lock_guard<std::mutex> Lock(m_mutex);
        if (m_shutting_down)
        {
            return UNBSE_SCRIPT_RESULT_SHUTTING_DOWN;
        }
        std::size_t OwnerCount{};
        for (const auto& Entry : m_entries)
        {
            if (!Entry.Occupied)
            {
                continue;
            }
            if (Entry.OwnerHandle == OwnerHandle)
            {
                if (Entry.Retiring)
                {
                    return UNBSE_SCRIPT_RESULT_OWNER_RETIRED;
                }
                ++OwnerCount;
            }
            if (Entry.NamespaceToken == Declaration->namespaceToken &&
                Entry.FunctionToken == Declaration->functionToken)
            {
                return UNBSE_SCRIPT_RESULT_DUPLICATE_NAME;
            }
        }
        if (OwnerCount >= UNBSE_SCRIPT_MAX_FUNCTIONS_PER_OWNER)
        {
            return UNBSE_SCRIPT_RESULT_OWNER_LIMIT;
        }
        if (m_entry_count >= m_entries.size())
        {
            return UNBSE_SCRIPT_RESULT_REGISTRY_FULL;
        }

        const auto Free = std::find_if(m_entries.begin(), m_entries.end(),
                                       [](const FEntry& Entry) { return !Entry.Occupied; });
        if (Free == m_entries.end())
        {
            return UNBSE_SCRIPT_RESULT_REGISTRY_FULL;
        }
        auto& Entry = *Free;
        Entry.Occupied = true;
        Entry.OwnerHandle = OwnerHandle;
        Entry.NamespaceToken = Declaration->namespaceToken;
        Entry.FunctionToken = Declaration->functionToken;
        Entry.Flags = Declaration->flags;
        Entry.ArgumentCount = Declaration->argumentCount;
        std::copy(std::begin(Declaration->argumentTypes), std::end(Declaration->argumentTypes), Entry.ArgumentTypes.begin());
        Entry.ResultType = Declaration->resultType;
        Entry.Callback = Declaration->callback;
        Entry.Context = Declaration->context;
        ++m_entry_count;
        return UNBSE_SCRIPT_RESULT_OK;
    }

    auto FScriptServiceRegistry::OwnerActiveCallsLocked(const std::uint32_t OwnerHandle) const -> std::size_t
    {
        std::size_t Active{};
        for (const auto& Entry : m_entries)
        {
            if (Entry.Occupied && Entry.OwnerHandle == OwnerHandle)
            {
                Active += Entry.ActiveCalls;
            }
        }
        return Active;
    }

    auto FScriptServiceRegistry::RemoveOwnerLocked(const std::uint32_t OwnerHandle) -> void
    {
        for (auto& Entry : m_entries)
        {
            if (Entry.Occupied && Entry.OwnerHandle == OwnerHandle)
            {
                Entry = {};
                --m_entry_count;
            }
        }
    }

    auto FScriptServiceRegistry::RetireOwner(
            const std::uint32_t OwnerHandle,
            const std::chrono::milliseconds Timeout) -> std::uint32_t
    {
        if (OwnerHandle == 0 || OwnerHandle == CoreOwnerHandle)
        {
            return UNBSE_SCRIPT_RESULT_OWNER_RETIRED;
        }
        std::unique_lock<std::mutex> Lock(m_mutex);
        for (auto& Entry : m_entries)
        {
            if (Entry.Occupied && Entry.OwnerHandle == OwnerHandle)
            {
                Entry.Retiring = true;
            }
        }
        const auto Deadline = std::chrono::steady_clock::now() + std::max(Timeout, std::chrono::milliseconds::zero());
        if (!m_changed.wait_until(Lock, Deadline, [this, OwnerHandle] { return OwnerActiveCallsLocked(OwnerHandle) == 0; }))
        {
            return UNBSE_SCRIPT_RESULT_RETIRE_TIMEOUT;
        }
        RemoveOwnerLocked(OwnerHandle);
        return UNBSE_SCRIPT_RESULT_OK;
    }

    auto FScriptServiceRegistry::FindVmLocked(const std::uintptr_t Identity) -> FVmBinding*
    {
        for (auto& Vm : m_vms)
        {
            if (Vm.Occupied && Vm.Identity == Identity)
            {
                return &Vm;
            }
        }
        return nullptr;
    }

    auto FScriptServiceRegistry::FindEntryLocked(
            const std::string& NamespaceToken,
            const std::string& FunctionToken) -> FEntry*
    {
        for (auto& Entry : m_entries)
        {
            if (Entry.Occupied && Entry.NamespaceToken == NamespaceToken && Entry.FunctionToken == FunctionToken)
            {
                return &Entry;
            }
        }
        return nullptr;
    }

    auto FScriptServiceRegistry::AttachVm(
            const std::uintptr_t VmIdentity,
            const std::uint32_t WindowsThreadId) -> std::uint32_t
    {
        if (VmIdentity == 0 || WindowsThreadId == 0)
        {
            return UNBSE_SCRIPT_RESULT_VM_UNAVAILABLE;
        }
        std::lock_guard<std::mutex> Lock(m_mutex);
        if (m_shutting_down)
        {
            return UNBSE_SCRIPT_RESULT_SHUTTING_DOWN;
        }
        if (const auto Existing = FindVmLocked(VmIdentity))
        {
            return Existing->WindowsThreadId == WindowsThreadId && !Existing->Detaching
                         ? UNBSE_SCRIPT_RESULT_OK
                         : UNBSE_SCRIPT_RESULT_VM_THREAD_MISMATCH;
        }
        if (m_vm_count >= m_vms.size())
        {
            return UNBSE_SCRIPT_RESULT_REGISTRY_FULL;
        }
        const auto Free = std::find_if(m_vms.begin(), m_vms.end(),
                                       [](const FVmBinding& Vm) { return !Vm.Occupied; });
        if (Free == m_vms.end())
        {
            return UNBSE_SCRIPT_RESULT_REGISTRY_FULL;
        }
        *Free = {true, VmIdentity, WindowsThreadId, 0, false};
        ++m_vm_count;
        return UNBSE_SCRIPT_RESULT_OK;
    }

    auto FScriptServiceRegistry::DetachVm(
            const std::uintptr_t VmIdentity,
            const std::chrono::milliseconds Timeout) -> std::uint32_t
    {
        std::unique_lock<std::mutex> Lock(m_mutex);
        const auto Binding = FindVmLocked(VmIdentity);
        if (!Binding)
        {
            return UNBSE_SCRIPT_RESULT_OK;
        }
        Binding->Detaching = true;
        const auto Deadline = std::chrono::steady_clock::now() + std::max(Timeout, std::chrono::milliseconds::zero());
        if (!m_changed.wait_until(Lock, Deadline, [Binding] { return Binding->ActiveCalls == 0; }))
        {
            return UNBSE_SCRIPT_RESULT_RETIRE_TIMEOUT;
        }
        *Binding = {};
        --m_vm_count;
        return UNBSE_SCRIPT_RESULT_OK;
    }

    auto FScriptServiceRegistry::Invoke(
            const std::uintptr_t VmIdentity,
            const std::uint32_t WindowsThreadId,
            const std::string& NamespaceToken,
            const std::string& FunctionToken,
            const UNBSEScriptValueV1* Arguments,
            const std::uint32_t ArgumentCount,
            const std::chrono::milliseconds Deadline) -> FScriptInvocationResult
    {
        return InvokeInternal(
                VmIdentity,
                WindowsThreadId,
                NamespaceToken,
                FunctionToken,
                Arguments,
                ArgumentCount,
                Deadline,
                true);
    }

    auto FScriptServiceRegistry::InvokeFromAttachedVmOnVerifiedThread(
            const std::uintptr_t VmIdentity,
            const std::uint32_t WindowsThreadId,
            const std::string& NamespaceToken,
            const std::string& FunctionToken,
            const UNBSEScriptValueV1* Arguments,
            const std::uint32_t ArgumentCount,
            const std::chrono::milliseconds Deadline) -> FScriptInvocationResult
    {
        return InvokeInternal(
                VmIdentity,
                WindowsThreadId,
                NamespaceToken,
                FunctionToken,
                Arguments,
                ArgumentCount,
                Deadline,
                false);
    }

    auto FScriptServiceRegistry::InvokeInternal(
            const std::uintptr_t VmIdentity,
            const std::uint32_t WindowsThreadId,
            const std::string& NamespaceToken,
            const std::string& FunctionToken,
            const UNBSEScriptValueV1* Arguments,
            const std::uint32_t ArgumentCount,
            const std::chrono::milliseconds Deadline,
            const bool RequireAttachmentThread) -> FScriptInvocationResult
    {
        UNBSEScriptFunctionV1 Callback{};
        void* Context{};
        std::uint64_t Sequence{};
        std::uint32_t ResultType{};
        FEntry* ActiveEntry{};
        FVmBinding* ActiveVm{};
        {
            std::lock_guard<std::mutex> Lock(m_mutex);
            if (m_shutting_down)
            {
                return MakeFailure(UNBSE_SCRIPT_RESULT_SHUTTING_DOWN, "script service is shutting down");
            }
            ActiveVm = FindVmLocked(VmIdentity);
            if (!ActiveVm || ActiveVm->Detaching)
            {
                return MakeFailure(UNBSE_SCRIPT_RESULT_VM_UNAVAILABLE, "script VM is not attached");
            }
            if (RequireAttachmentThread && ActiveVm->WindowsThreadId != WindowsThreadId)
            {
                return MakeFailure(UNBSE_SCRIPT_RESULT_VM_THREAD_MISMATCH, "script VM thread changed");
            }
            ActiveEntry = FindEntryLocked(NamespaceToken, FunctionToken);
            if (!ActiveEntry || ActiveEntry->Retiring)
            {
                return MakeFailure(UNBSE_SCRIPT_RESULT_FUNCTION_NOT_FOUND, "function is not registered");
            }
            if (ArgumentCount != ActiveEntry->ArgumentCount || (ArgumentCount != 0 && !Arguments))
            {
                return MakeFailure(UNBSE_SCRIPT_RESULT_ARGUMENT_MISMATCH, "argument count differs from declaration");
            }
            for (std::size_t Index = 0; Index < ArgumentCount; ++Index)
            {
                if (!IsCanonicalValue(Arguments[Index], ActiveEntry->ArgumentTypes[Index]))
                {
                    return MakeFailure(UNBSE_SCRIPT_RESULT_ARGUMENT_MISMATCH, "argument type or value is malformed");
                }
            }
            Callback = ActiveEntry->Callback;
            Context = ActiveEntry->Context;
            Sequence = m_next_sequence++;
            ResultType = ActiveEntry->ResultType;
            ++ActiveEntry->ActiveCalls;
            ++ActiveVm->ActiveCalls;
        }

        UNBSEScriptInvocationV1 Invocation{
                sizeof(UNBSEScriptInvocationV1),
                UNBSE_SCRIPT_SERVICE_ABI_VERSION,
                ArgumentCount,
                static_cast<std::uint32_t>(std::max(Deadline, std::chrono::milliseconds::zero()).count()),
                WindowsThreadId,
                0,
                Sequence,
                Arguments,
        };
        UNBSEScriptResultV1 RawResult{};
        RawResult.structSize = sizeof(UNBSEScriptResultV1);
        RawResult.apiVersion = UNBSE_SCRIPT_SERVICE_ABI_VERSION;
        RawResult.code = UNBSE_SCRIPT_RESULT_CALLBACK_FAILED;
        RawResult.value.structSize = sizeof(UNBSEScriptValueV1);
        RawResult.value.type = UNBSE_SCRIPT_VALUE_NONE;
        const auto Started = std::chrono::steady_clock::now();
        try
        {
            Callback(&Invocation, &RawResult, Context);
        }
        catch (const std::exception&)
        {
            RawResult.code = UNBSE_SCRIPT_RESULT_CALLBACK_FAILED;
        }
        catch (...)
        {
            RawResult.code = UNBSE_SCRIPT_RESULT_CALLBACK_FAILED;
        }
        const auto Elapsed = std::chrono::steady_clock::now() - Started;

        {
            std::lock_guard<std::mutex> Lock(m_mutex);
            --ActiveEntry->ActiveCalls;
            --ActiveVm->ActiveCalls;
            m_changed.notify_all();
        }

        if (Elapsed > std::max(Deadline, std::chrono::milliseconds::zero()))
        {
            auto Result = MakeFailure(
                    UNBSE_SCRIPT_RESULT_DEADLINE_EXPIRED,
                    "function exceeded its synchronous deadline");
            Result.Sequence = Sequence;
            Result.ElapsedMilliseconds = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed).count());
            return Result;
        }
        if (RawResult.structSize != sizeof(UNBSEScriptResultV1) ||
            RawResult.apiVersion != UNBSE_SCRIPT_SERVICE_ABI_VERSION || RawResult.reserved != 0 ||
            !IsKnownResultCode(RawResult.code))
        {
            return MakeFailure(UNBSE_SCRIPT_RESULT_CALLBACK_FAILED, "function returned a malformed result");
        }
        const auto DiagnosticEnd = static_cast<const char*>(
                std::memchr(RawResult.diagnostic, '\0', UNBSE_SCRIPT_DIAGNOSTIC_BYTES));
        if (!DiagnosticEnd)
        {
            return MakeFailure(UNBSE_SCRIPT_RESULT_CALLBACK_FAILED, "function returned an unterminated diagnostic");
        }
        if (RawResult.code != UNBSE_SCRIPT_RESULT_OK)
        {
            if (!IsCanonicalValue(RawResult.value, UNBSE_SCRIPT_VALUE_NONE))
            {
                return MakeFailure(UNBSE_SCRIPT_RESULT_CALLBACK_FAILED, "function returned a malformed failure value");
            }
            auto Result = MakeFailure(
                    RawResult.code,
                    std::string(RawResult.diagnostic,
                                static_cast<std::size_t>(DiagnosticEnd - RawResult.diagnostic)));
            Result.Sequence = Sequence;
            Result.ElapsedMilliseconds = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed).count());
            return Result;
        }
        if (!IsCanonicalValue(RawResult.value, ResultType))
        {
            return MakeFailure(UNBSE_SCRIPT_RESULT_CALLBACK_FAILED, "function returned a malformed success value");
        }
        FScriptInvocationResult Result{};
        Result.Code = RawResult.code;
        Result.Sequence = Sequence;
        Result.ElapsedMilliseconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed).count());
        Result.Value = RawResult.value;
        Result.Diagnostic.assign(
                RawResult.diagnostic,
                static_cast<std::size_t>(DiagnosticEnd - RawResult.diagnostic));
        return Result;
    }

    auto FScriptServiceRegistry::BeginShutdown(const std::chrono::milliseconds Timeout) -> std::uint32_t
    {
        std::unique_lock<std::mutex> Lock(m_mutex);
        m_shutting_down = true;
        for (auto& Entry : m_entries)
        {
            if (Entry.Occupied)
            {
                Entry.Retiring = true;
            }
        }
        for (auto& Vm : m_vms)
        {
            if (Vm.Occupied)
            {
                Vm.Detaching = true;
            }
        }
        const auto NoActiveCalls = [this] {
            return std::all_of(m_entries.begin(), m_entries.end(),
                               [](const FEntry& Entry) { return !Entry.Occupied || Entry.ActiveCalls == 0; }) &&
                   std::all_of(m_vms.begin(), m_vms.end(),
                               [](const FVmBinding& Vm) { return !Vm.Occupied || Vm.ActiveCalls == 0; });
        };
        const auto Deadline = std::chrono::steady_clock::now() + std::max(Timeout, std::chrono::milliseconds::zero());
        if (!m_changed.wait_until(Lock, Deadline, NoActiveCalls))
        {
            return UNBSE_SCRIPT_RESULT_RETIRE_TIMEOUT;
        }
        for (auto& Entry : m_entries)
        {
            Entry = {};
        }
        for (auto& Vm : m_vms)
        {
            Vm = {};
        }
        m_entry_count = 0;
        m_vm_count = 0;
        m_tes_identity_snapshot_available = false;
        m_guid_xaag_handlers_available = false;
        m_area_runtime_snapshot_available = false;
        return UNBSE_SCRIPT_RESULT_OK;
    }

    void FScriptServiceRegistry::SetTesIdentitySnapshotAvailable(const bool Available)
    {
        std::lock_guard<std::mutex> Lock(m_mutex);
        if (!m_shutting_down)
        {
            m_tes_identity_snapshot_available = Available;
        }
    }

    void FScriptServiceRegistry::SetGuidXaagHandlersAvailable(const bool Available)
    {
        std::lock_guard<std::mutex> Lock(m_mutex);
        if (!m_shutting_down)
        {
            m_guid_xaag_handlers_available = Available;
        }
    }

    void FScriptServiceRegistry::SetAreaRuntimeSnapshotAvailable(const bool Available)
    {
        std::lock_guard<std::mutex> Lock(m_mutex);
        if (!m_shutting_down)
        {
            m_area_runtime_snapshot_available = Available;
        }
    }

    void FScriptServiceRegistry::SetAreaSplineSnapshotAvailable(const bool Available)
    {
        std::lock_guard<std::mutex> Lock(m_mutex);
        if (!m_shutting_down)
        {
            m_area_spline_snapshot_available = Available;
        }
    }

    void FScriptServiceRegistry::SetMaterialParameterCollectionsAvailable(const bool Available)
    {
        std::lock_guard<std::mutex> Lock(m_mutex);
        if (!m_shutting_down)
        {
            m_material_parameter_collections_available = Available;
        }
    }

    void FScriptServiceRegistry::SetRvtProducersAvailable(const bool Available)
    {
        std::lock_guard<std::mutex> Lock(m_mutex);
        if (!m_shutting_down)
        {
            m_rvt_producers_available = Available;
        }
    }

    auto FScriptServiceRegistry::Capabilities() const -> std::uint32_t
    {
        std::lock_guard<std::mutex> Lock(m_mutex);
        if (m_shutting_down)
        {
            return 0;
        }
        auto Result = static_cast<std::uint32_t>(UNBSE_SCRIPT_CAPABILITY_DECLARATION_REGISTRY);
        if (m_vm_count != 0)
        {
            Result |= UNBSE_SCRIPT_CAPABILITY_UE4SS_LUA_VM;
        }
        if (m_tes_identity_snapshot_available)
        {
            Result |= UNBSE_SCRIPT_CAPABILITY_TES_IDENTITY_SNAPSHOT;
        }
        if (m_guid_xaag_handlers_available && m_tes_identity_snapshot_available)
        {
            Result |= UNBSE_SCRIPT_CAPABILITY_GUID_XAAG_HANDLERS;
        }
        if (m_area_runtime_snapshot_available)
        {
            Result |= UNBSE_SCRIPT_CAPABILITY_AREA_RUNTIME_SNAPSHOT;
        }
        if (m_area_spline_snapshot_available && m_area_runtime_snapshot_available)
        {
            Result |= UNBSE_SCRIPT_CAPABILITY_AREA_SPLINE_SNAPSHOT;
        }
        if (m_material_parameter_collections_available && m_area_runtime_snapshot_available)
        {
            Result |= UNBSE_SCRIPT_CAPABILITY_MATERIAL_PARAMETER_COLLECTIONS;
        }
        if (m_rvt_producers_available && m_area_runtime_snapshot_available)
        {
            Result |= UNBSE_SCRIPT_CAPABILITY_RVT_PRODUCERS;
        }
        return Result;
    }

    auto FScriptServiceRegistry::FunctionCount() const -> std::size_t
    {
        std::lock_guard<std::mutex> Lock(m_mutex);
        return m_entry_count;
    }

    auto FScriptServiceRegistry::VmCount() const -> std::size_t
    {
        std::lock_guard<std::mutex> Lock(m_mutex);
        return m_vm_count;
    }

    auto PublishScriptServiceRegistry(FScriptServiceRegistry* Registry) -> bool
    {
        if (!Registry)
        {
            return false;
        }
        FScriptServiceRegistry* Expected{};
        return PublishedRegistry.compare_exchange_strong(Expected, Registry, std::memory_order_acq_rel);
    }

    auto UnpublishScriptServiceRegistry(FScriptServiceRegistry* Registry) -> void
    {
        PublishedRegistry.compare_exchange_strong(Registry, nullptr, std::memory_order_acq_rel);
    }

    auto GetPublishedScriptServiceRegistry() -> FScriptServiceRegistry*
    {
        return PublishedRegistry.load(std::memory_order_acquire);
    }
} // namespace RC::UNBSE

extern "C" UNBSE_SCRIPT_EXPORT int32_t UNBSE_SCRIPT_CALL UNBSE_QueryScriptServiceV1(
        const uint32_t RequestedVersion,
        UNBSEScriptServiceV1* Service)
{
    if (!Service || RequestedVersion != UNBSE_SCRIPT_SERVICE_ABI_VERSION ||
        Service->structSize != sizeof(UNBSEScriptServiceV1))
    {
        return 0;
    }
    *Service = {
            sizeof(UNBSEScriptServiceV1),
            UNBSE_SCRIPT_SERVICE_ABI_VERSION,
            &RC::UNBSE::GetCapabilitiesThunk,
            &RC::UNBSE::RegisterFunctionThunk,
            &RC::UNBSE::RetireOwnerThunk,
            {},
    };
    return 1;
}
