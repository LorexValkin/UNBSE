#pragma once

#include <UNBSEScriptServiceV1.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace RC::UNBSE
{
    struct FScriptInvocationResult
    {
        std::uint32_t Code{UNBSE_SCRIPT_RESULT_CALLBACK_FAILED};
        std::uint64_t Sequence{};
        std::uint64_t ElapsedMilliseconds{};
        UNBSEScriptValueV1 Value{};
        std::string Diagnostic{};
    };

    class FScriptServiceRegistry final
    {
      public:
        static constexpr std::uint32_t CoreOwnerHandle = 1;
        static constexpr std::size_t MaxVmBindings = 4096;

        FScriptServiceRegistry() = default;
        ~FScriptServiceRegistry();
        FScriptServiceRegistry(const FScriptServiceRegistry&) = delete;
        FScriptServiceRegistry& operator=(const FScriptServiceRegistry&) = delete;

        auto Register(std::uint32_t OwnerHandle, const UNBSEScriptFunctionDeclarationV1* Declaration)
                -> std::uint32_t;
        auto RetireOwner(std::uint32_t OwnerHandle, std::chrono::milliseconds Timeout)
                -> std::uint32_t;
        auto AttachVm(std::uintptr_t VmIdentity, std::uint32_t WindowsThreadId) -> std::uint32_t;
        auto DetachVm(std::uintptr_t VmIdentity, std::chrono::milliseconds Timeout) -> std::uint32_t;
        auto Invoke(std::uintptr_t VmIdentity,
                    std::uint32_t WindowsThreadId,
                    const std::string& NamespaceToken,
                    const std::string& FunctionToken,
                    const UNBSEScriptValueV1* Arguments,
                    std::uint32_t ArgumentCount,
                    std::chrono::milliseconds Deadline) -> FScriptInvocationResult;
        // Internal fixed bindings may use a designated execution thread. The
        // exact VM must still be attached, and DetachVm waits for the active
        // call lease. Callbacks remain responsible for enforcing their
        // engine-access boundary.
        auto InvokeFromAttachedVmOnVerifiedThread(
                    std::uintptr_t VmIdentity,
                    std::uint32_t WindowsThreadId,
                    const std::string& NamespaceToken,
                    const std::string& FunctionToken,
                    const UNBSEScriptValueV1* Arguments,
                    std::uint32_t ArgumentCount,
                    std::chrono::milliseconds Deadline) -> FScriptInvocationResult;
        auto BeginShutdown(std::chrono::milliseconds Timeout) -> std::uint32_t;
        void SetTesIdentitySnapshotAvailable(bool Available);
        void SetGuidXaagHandlersAvailable(bool Available);
        void SetAreaRuntimeSnapshotAvailable(bool Available);
        void SetAreaSplineSnapshotAvailable(bool Available);
        void SetMaterialParameterCollectionsAvailable(bool Available);
        void SetRvtProducersAvailable(bool Available);
        auto Capabilities() const -> std::uint32_t;
        auto FunctionCount() const -> std::size_t;
        auto VmCount() const -> std::size_t;

      private:
        struct FEntry
        {
            bool Occupied{};
            std::uint32_t OwnerHandle{};
            std::string NamespaceToken{};
            std::string FunctionToken{};
            std::uint32_t Flags{};
            std::uint32_t ArgumentCount{};
            std::array<std::uint32_t, UNBSE_SCRIPT_MAX_ARGUMENTS> ArgumentTypes{};
            std::uint32_t ResultType{};
            UNBSEScriptFunctionV1 Callback{};
            void* Context{};
            std::size_t ActiveCalls{};
            bool Retiring{};
        };

        struct FVmBinding
        {
            bool Occupied{};
            std::uintptr_t Identity{};
            std::uint32_t WindowsThreadId{};
            std::size_t ActiveCalls{};
            bool Detaching{};
        };

        auto OwnerActiveCallsLocked(std::uint32_t OwnerHandle) const -> std::size_t;
        auto RemoveOwnerLocked(std::uint32_t OwnerHandle) -> void;
        auto FindVmLocked(std::uintptr_t Identity) -> FVmBinding*;
        auto FindEntryLocked(const std::string& NamespaceToken, const std::string& FunctionToken) -> FEntry*;
        auto InvokeInternal(std::uintptr_t VmIdentity,
                            std::uint32_t WindowsThreadId,
                            const std::string& NamespaceToken,
                            const std::string& FunctionToken,
                            const UNBSEScriptValueV1* Arguments,
                            std::uint32_t ArgumentCount,
                            std::chrono::milliseconds Deadline,
                            bool RequireAttachmentThread) -> FScriptInvocationResult;

        mutable std::mutex m_mutex{};
        std::condition_variable m_changed{};
        std::array<FEntry, UNBSE_SCRIPT_MAX_FUNCTIONS> m_entries{};
        std::size_t m_entry_count{};
        std::array<FVmBinding, MaxVmBindings> m_vms{};
        std::size_t m_vm_count{};
        std::uint64_t m_next_sequence{1};
        bool m_tes_identity_snapshot_available{};
        bool m_guid_xaag_handlers_available{};
        bool m_area_runtime_snapshot_available{};
        bool m_area_spline_snapshot_available{};
        bool m_material_parameter_collections_available{};
        bool m_rvt_producers_available{};
        bool m_shutting_down{};
    };

    auto PublishScriptServiceRegistry(FScriptServiceRegistry* Registry) -> bool;
    auto UnpublishScriptServiceRegistry(FScriptServiceRegistry* Registry) -> void;
    auto GetPublishedScriptServiceRegistry() -> FScriptServiceRegistry*;
} // namespace RC::UNBSE

extern "C" UNBSE_SCRIPT_EXPORT int32_t UNBSE_SCRIPT_CALL UNBSE_QueryScriptServiceV1(
        uint32_t RequestedVersion,
        UNBSEScriptServiceV1* Service);
