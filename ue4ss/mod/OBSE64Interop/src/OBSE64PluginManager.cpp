#include <OBSE64PluginManager.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <Windows.h>

namespace RC::UNBSE::OBSE64
{
    namespace
    {
        auto EscapeJson(const std::string& Value) -> std::string
        {
            std::string Result{};
            Result.reserve(Value.size());
            constexpr char Digits[] = "0123456789ABCDEF";
            for (const auto Character : Value)
            {
                const auto Byte = static_cast<unsigned char>(Character);
                switch (Character)
                {
                case '"': Result += "\\\""; break;
                case '\\': Result += "\\\\"; break;
                case '\b': Result += "\\b"; break;
                case '\f': Result += "\\f"; break;
                case '\n': Result += "\\n"; break;
                case '\r': Result += "\\r"; break;
                case '\t': Result += "\\t"; break;
                default:
                    if (Byte < 0x20)
                    {
                        Result += "\\u00";
                        Result.push_back(Digits[(Byte >> 4) & 0xF]);
                        Result.push_back(Digits[Byte & 0xF]);
                    }
                    else
                    {
                        Result.push_back(Character);
                    }
                    break;
                }
            }
            return Result;
        }

        auto FoldAscii(std::string Value) -> std::string
        {
            std::transform(Value.begin(), Value.end(), Value.begin(), [](const char Character) {
                return Character >= 'A' && Character <= 'Z'
                               ? static_cast<char>(Character - 'A' + 'a')
                               : Character;
            });
            return Value;
        }

        constexpr std::size_t TrampolinePoolCapacity = 64u * 1024u;
        constexpr std::size_t AbsoluteJumpBytes = 14u;

        auto ModuleImageSize(const HMODULE Module) -> std::size_t
        {
            if (!Module)
            {
                return 0;
            }
            const auto Base = reinterpret_cast<const std::byte*>(Module);
            const auto Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
            if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0)
            {
                return 0;
            }
            const auto Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
            if (Nt->Signature != IMAGE_NT_SIGNATURE ||
                Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            {
                return 0;
            }
            return Nt->OptionalHeader.SizeOfImage;
        }

        auto ModuleForAddress(const void* Address) -> HMODULE
        {
            MEMORY_BASIC_INFORMATION Info{};
            if (!Address || VirtualQuery(Address, &Info, sizeof(Info)) != sizeof(Info))
            {
                return nullptr;
            }
            return static_cast<HMODULE>(Info.AllocationBase);
        }

        auto AlignDown(const std::uintptr_t Value, const std::uintptr_t Alignment)
                -> std::uintptr_t
        {
            return Alignment == 0 ? Value : Value - (Value % Alignment);
        }

        auto IsReadableRange(const std::uintptr_t Address, const std::size_t Bytes) -> bool
        {
            if (Address == 0 || Bytes == 0 ||
                Address > std::numeric_limits<std::uintptr_t>::max() - Bytes)
            {
                return false;
            }
            MEMORY_BASIC_INFORMATION Info{};
            if (VirtualQuery(reinterpret_cast<const void*>(Address), &Info, sizeof(Info)) !=
                sizeof(Info))
            {
                return false;
            }
            if (Info.State != MEM_COMMIT || (Info.Protect & PAGE_GUARD) != 0 ||
                (Info.Protect & PAGE_NOACCESS) != 0)
            {
                return false;
            }
            const auto RegionBase = reinterpret_cast<std::uintptr_t>(Info.BaseAddress);
            return Address >= RegionBase &&
                   Bytes <= Info.RegionSize - static_cast<std::size_t>(Address - RegionBase);
        }

        auto ReadExpectedDirectCall(const std::uintptr_t CallSite,
                                    const std::uintptr_t ExpectedTarget,
                                    std::array<std::uint8_t, 5>* OriginalCall = nullptr) -> bool
        {
            if (!IsReadableRange(CallSite, 5) || ExpectedTarget == 0)
            {
                return false;
            }
            std::array<std::uint8_t, 5> Original{};
            std::memcpy(Original.data(), reinterpret_cast<const void*>(CallSite), Original.size());
            if (Original[0] != 0xE8)
            {
                return false;
            }
            std::int32_t OriginalDisplacement{};
            std::memcpy(&OriginalDisplacement, Original.data() + 1,
                        sizeof(OriginalDisplacement));
            const auto DecodedTarget = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(CallSite + Original.size()) +
                    OriginalDisplacement);
            if (DecodedTarget != ExpectedTarget)
            {
                return false;
            }
            if (OriginalCall)
            {
                *OriginalCall = Original;
            }
            return true;
        }

        auto WriteCode(const std::uintptr_t Address, const void* Data, const std::size_t Bytes)
                -> bool
        {
            if (!Data || !IsReadableRange(Address, Bytes))
            {
                return false;
            }
            auto Destination = reinterpret_cast<void*>(Address);
            DWORD PreviousProtection{};
            if (!VirtualProtect(Destination, Bytes, PAGE_EXECUTE_READWRITE,
                                &PreviousProtection))
            {
                return false;
            }
            std::memcpy(Destination, Data, Bytes);
            const auto Flushed = FlushInstructionCache(GetCurrentProcess(), Destination, Bytes) != 0;
            DWORD IgnoredProtection{};
            const auto Restored = VirtualProtect(Destination, Bytes, PreviousProtection,
                                                 &IgnoredProtection) != 0;
            return Flushed && Restored;
        }

        class FExecutablePool final
        {
          public:
            FExecutablePool() = default;
            ~FExecutablePool() { Release(); }
            FExecutablePool(const FExecutablePool&) = delete;
            FExecutablePool& operator=(const FExecutablePool&) = delete;

            auto Initialize(const HMODULE AnchorModule,
                            const std::size_t Capacity = TrampolinePoolCapacity) -> bool
            {
                std::lock_guard<std::mutex> Lock(Mutex);
                if (Base || !AnchorModule || Capacity == 0)
                {
                    return false;
                }
                SYSTEM_INFO SystemInfo{};
                GetSystemInfo(&SystemInfo);
                const auto Granularity = static_cast<std::uintptr_t>(
                        SystemInfo.dwAllocationGranularity);
                const auto ImageSize = ModuleImageSize(AnchorModule);
                const auto Anchor = reinterpret_cast<std::uintptr_t>(AnchorModule);
                const auto Rel32Maximum = static_cast<std::uintptr_t>(
                        std::numeric_limits<std::int32_t>::max());
                if (Granularity == 0 || ImageSize == 0 ||
                    ImageSize > Rel32Maximum - Capacity - Granularity)
                {
                    return false;
                }
                const auto MaximumDistance =
                        Rel32Maximum - ImageSize - Capacity - Granularity;
                const auto LowestAddress = Anchor > MaximumDistance
                                                    ? Anchor - MaximumDistance
                                                    : Granularity;
                auto Cursor = Anchor;
                while (Cursor > LowestAddress)
                {
                    MEMORY_BASIC_INFORMATION Info{};
                    if (VirtualQuery(reinterpret_cast<const void*>(Cursor - 1), &Info,
                                     sizeof(Info)) != sizeof(Info))
                    {
                        break;
                    }
                    const auto RegionBase =
                            reinterpret_cast<std::uintptr_t>(Info.BaseAddress);
                    const auto RegionBytes = static_cast<std::uintptr_t>(Info.RegionSize);
                    if (RegionBase > std::numeric_limits<std::uintptr_t>::max() - RegionBytes)
                    {
                        break;
                    }
                    const auto RegionEnd = std::min(Cursor, RegionBase + RegionBytes);
                    if (Info.State == MEM_FREE && RegionEnd >= Capacity)
                    {
                        const auto Candidate = AlignDown(
                                RegionEnd - static_cast<std::uintptr_t>(Capacity), Granularity);
                        if (Candidate >= LowestAddress && Candidate < Anchor)
                        {
                            const auto Allocation = VirtualAlloc(
                                    reinterpret_cast<void*>(Candidate), Capacity,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                            if (Allocation)
                            {
                                Base = static_cast<std::byte*>(Allocation);
                                Size = Capacity;
                                Used = 0;
                                return true;
                            }
                        }
                    }
                    if (RegionBase == 0 || RegionBase >= Cursor)
                    {
                        break;
                    }
                    Cursor = RegionBase;
                }
                return false;
            }

            auto Allocate(const std::size_t Bytes) -> void*
            {
                std::lock_guard<std::mutex> Lock(Mutex);
                if (!Base || Bytes > Size - Used)
                {
                    return nullptr;
                }
                // OBSE64 returns the current pool cursor for a zero-byte request.
                auto Result = Base + Used;
                Used += Bytes;
                return Result;
            }

            auto Available() const -> bool { return Base != nullptr; }

            auto Release() -> void
            {
                std::lock_guard<std::mutex> Lock(Mutex);
                if (Base)
                {
                    VirtualFree(Base, 0, MEM_RELEASE);
                }
                Base = nullptr;
                Size = 0;
                Used = 0;
            }

          private:
            mutable std::mutex Mutex{};
            std::byte* Base{};
            std::size_t Size{};
            std::size_t Used{};
        };
    } // namespace

    struct FPluginManager::FImpl
    {
        enum class EState
        {
            Accepted,
            Loading,
            Loaded,
            Failed
        };

        struct FRecord
        {
            std::filesystem::path Path{};
            FPluginScanResult Scan{};
            FPluginCompatibilityResult Compatibility{};
            PluginHandle Handle{InvalidPluginHandle};
            HMODULE Module{};
            EState State{EState::Accepted};
            FPluginInfo Info{};
        };

        struct FListener
        {
            PluginHandle Listener{InvalidPluginHandle};
            FMessagingInterface::FEventCallback Callback{};
        };

        static std::atomic<FImpl*> Published;
        static std::atomic<std::uintptr_t> PublishedDataLoadedOriginal;
        static thread_local PluginHandle CurrentLoadingHandle;

        explicit FImpl(FPluginManagerConfig InConfig, FPluginManagerEventSink InEventSink)
            : Config(std::move(InConfig)), EventSink(std::move(InEventSink))
        {
            Interface = {
                    Config.EmulatedScriptExtenderVersion,
                    Config.RuntimeVersion,
                    0,
                    0,
                    InterfaceVersion,
                    &QueryInterfaceThunk,
                    &GetPluginHandleThunk,
                    &GetReleaseIndexThunk,
                    &GetPluginInfoThunk,
                    &GetSaveFolderNameThunk,
            };
            Messaging = {
                    MessagingInterfaceVersion,
                    &RegisterListenerThunk,
                    &DispatchThunk,
            };
            Trampoline = {
                    TrampolineInterfaceVersion,
                    &AllocateFromBranchPoolThunk,
                    &AllocateFromLocalPoolThunk,
            };
            const auto BranchReady = BranchPool.Initialize(GetModuleHandleW(nullptr));
            const auto LocalReady = LocalPool.Initialize(ModuleForAddress(&Published));
            TrampolineReady = BranchReady && LocalReady;
            if (!TrampolineReady)
            {
                BranchPool.Release();
                LocalPool.Release();
            }
            Emit("trampoline", nullptr,
                 TrampolineReady ? "branch-local-pools-ready" : "pool-initialization-failed");
        }

        auto Emit(const char* Event,
                  const FRecord* Record,
                  const char* Status,
                  const std::uint32_t Detail = 0) const -> void
        {
            if (!EventSink)
            {
                return;
            }
            std::ostringstream Json{};
            Json << "{\"schema\":\"UNBSE.OBSE64Interop\",\"schemaVersion\":1,"
                    "\"event\":\""
                 << Event << "\",\"status\":\"" << Status << '"';
            if (Record)
            {
                Json << ",\"plugin\":\"" << EscapeJson(Record->Scan.Name)
                     << "\",\"path\":\"" << EscapeJson(Record->Path.string())
                     << "\",\"handle\":" << Record->Handle
                     << ",\"compatibility\":\""
                     << PluginCompatibilityDispositionName(Record->Compatibility.Disposition)
                     << "\",\"compatibilityReasons\":" << Record->Compatibility.Reasons;
            }
            if (Detail != 0)
            {
                Json << ",\"detail\":" << Detail;
            }
            Json << '}';
            EventSink(Json.str());
        }

        auto AllocateTrampoline(const bool Branch,
                                const PluginHandle Plugin,
                                const std::size_t Bytes) -> void*
        {
            void* Result{};
            {
                std::lock_guard<std::mutex> Lock(Mutex);
                if (ShuttingDown || !TrampolineReady || !IsCallableOwnerLocked(Plugin))
                {
                    return nullptr;
                }
                Result = (Branch ? BranchPool : LocalPool).Allocate(Bytes);
            }
            Emit("trampoline-allocation", nullptr,
                 Result ? (Branch ? "branch-allocated" : "local-allocated")
                        : (Branch ? "branch-exhausted" : "local-exhausted"),
                 Bytes <= std::numeric_limits<std::uint32_t>::max()
                         ? static_cast<std::uint32_t>(Bytes)
                         : std::numeric_limits<std::uint32_t>::max());
            return Result;
        }

        auto DispatchDataLoadedOnce() -> bool
        {
            if (DataLoadedWasDispatched.exchange(true, std::memory_order_acq_rel))
            {
                Emit("lifecycle", nullptr, "data-loaded-duplicate-withheld");
                return false;
            }
            const auto Result = Dispatch(
                    CorePluginHandle, MessageDataLoaded, nullptr, 0, nullptr);
            Emit("lifecycle", nullptr,
                 Result ? "data-loaded-dispatched" : "data-loaded-no-listeners");
            return Result;
        }

        auto InstallDataLoadedHook(const std::uintptr_t CallSite,
                                   const std::uintptr_t ExpectedTarget) -> bool
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            if (ShuttingDown || !IsPublished || !TrampolineReady || DataLoadedHookActive ||
                CallSite == 0 || ExpectedTarget == 0)
            {
                return false;
            }

            std::array<std::uint8_t, 5> Original{};
            if (!ReadExpectedDirectCall(CallSite, ExpectedTarget, &Original))
            {
                return false;
            }

            auto Relay = static_cast<std::uint8_t*>(BranchPool.Allocate(AbsoluteJumpBytes));
            if (!Relay)
            {
                return false;
            }
            const auto Hook = &DataLoadedHookThunk;
            std::uintptr_t HookAddress{};
            static_assert(sizeof(Hook) == sizeof(HookAddress));
            std::memcpy(&HookAddress, &Hook, sizeof(HookAddress));
            const std::array<std::uint8_t, 6> AbsoluteJumpPrefix{0xFF, 0x25, 0, 0, 0, 0};
            std::memcpy(Relay, AbsoluteJumpPrefix.data(), AbsoluteJumpPrefix.size());
            std::memcpy(Relay + AbsoluteJumpPrefix.size(), &HookAddress, sizeof(HookAddress));
            if (!FlushInstructionCache(GetCurrentProcess(), Relay, AbsoluteJumpBytes))
            {
                return false;
            }

            const auto RelayAddress = reinterpret_cast<std::uintptr_t>(Relay);
            const auto Delta = static_cast<std::int64_t>(RelayAddress) -
                               static_cast<std::int64_t>(CallSite + Original.size());
            if (Delta < std::numeric_limits<std::int32_t>::min() ||
                Delta > std::numeric_limits<std::int32_t>::max())
            {
                return false;
            }
            auto Patched = Original;
            const auto Relative = static_cast<std::int32_t>(Delta);
            std::memcpy(Patched.data() + 1, &Relative, sizeof(Relative));

            std::uintptr_t EmptyOriginal{};
            if (!PublishedDataLoadedOriginal.compare_exchange_strong(
                        EmptyOriginal, ExpectedTarget, std::memory_order_acq_rel))
            {
                return false;
            }
            if (!WriteCode(CallSite, Patched.data(), Patched.size()))
            {
                PublishedDataLoadedOriginal.store(0, std::memory_order_release);
                return false;
            }

            DataLoadedCallSite = CallSite;
            OriginalDataLoadedCall = Original;
            PatchedDataLoadedCall = Patched;
            DataLoadedHookActive = true;
            return true;
        }

        auto RemoveDataLoadedHook() -> bool
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            if (!DataLoadedHookActive)
            {
                return true;
            }
            if (!IsReadableRange(DataLoadedCallSite, PatchedDataLoadedCall.size()) ||
                std::memcmp(reinterpret_cast<const void*>(DataLoadedCallSite),
                            PatchedDataLoadedCall.data(), PatchedDataLoadedCall.size()) != 0)
            {
                DataLoadedHookActive = false;
                PublishedDataLoadedOriginal.store(0, std::memory_order_release);
                return false;
            }
            if (!WriteCode(DataLoadedCallSite, OriginalDataLoadedCall.data(),
                           OriginalDataLoadedCall.size()))
            {
                return false;
            }
            DataLoadedHookActive = false;
            DataLoadedCallSite = 0;
            PublishedDataLoadedOriginal.store(0, std::memory_order_release);
            return true;
        }

        auto FindRecordByHandleLocked(const PluginHandle Handle) -> FRecord*
        {
            const auto Iterator = std::find_if(Records.begin(), Records.end(),
                                               [Handle](const FRecord& Record) {
                                                   return Record.Handle == Handle;
                                               });
            return Iterator == Records.end() ? nullptr : &*Iterator;
        }

        auto FindRecordByNameLocked(const char* Name) -> FRecord*
        {
            if (!Name)
            {
                return nullptr;
            }
            const auto Folded = FoldAscii(Name);
            const auto Iterator = std::find_if(Records.begin(), Records.end(),
                                               [&Folded](const FRecord& Record) {
                                                   return FoldAscii(Record.Scan.Name) == Folded &&
                                                          Record.State != EState::Failed;
                                               });
            return Iterator == Records.end() ? nullptr : &*Iterator;
        }

        auto IsCallableOwnerLocked(const PluginHandle Handle) -> bool
        {
            if (Handle == CorePluginHandle)
            {
                return true;
            }
            const auto Record = FindRecordByHandleLocked(Handle);
            return Record && (Record->State == EState::Loading || Record->State == EState::Loaded);
        }

        auto NameForHandleLocked(const PluginHandle Handle) -> const char*
        {
            if (Handle == CorePluginHandle)
            {
                return "OBSE";
            }
            const auto Record = FindRecordByHandleLocked(Handle);
            return Record && Record->State != EState::Failed ? Record->Scan.Name.c_str() : nullptr;
        }

        auto RegisterListener(const PluginHandle Listener,
                              const char* Sender,
                              const FMessagingInterface::FEventCallback Callback) -> bool
        {
            if (!Callback)
            {
                return false;
            }
            std::lock_guard<std::mutex> Lock(Mutex);
            if (ShuttingDown || !IsCallableOwnerLocked(Listener))
            {
                return false;
            }

            std::vector<PluginHandle> SenderHandles{};
            if (Sender)
            {
                if (FoldAscii(Sender) == "obse")
                {
                    SenderHandles.push_back(CorePluginHandle);
                }
                else
                {
                    const auto Record = FindRecordByNameLocked(Sender);
                    if (!Record)
                    {
                        return false;
                    }
                    SenderHandles.push_back(Record->Handle);
                }
            }
            else
            {
                for (const auto& Record : Records)
                {
                    if (Record.State != EState::Failed && Record.Handle != Listener)
                    {
                        SenderHandles.push_back(Record.Handle);
                    }
                }
            }

            for (const auto SenderHandle : SenderHandles)
            {
                auto& Entries = Listeners[SenderHandle];
                const auto Duplicate = std::find_if(
                        Entries.begin(), Entries.end(),
                        [Listener](const FListener& Entry) { return Entry.Listener == Listener; });
                if (Duplicate == Entries.end())
                {
                    Entries.push_back({Listener, Callback});
                }
            }
            return true;
        }

        auto Dispatch(const PluginHandle Sender,
                      const std::uint32_t MessageType,
                      void* Data,
                      const std::uint32_t DataLength,
                      const char* Receiver) -> bool
        {
            std::vector<FListener> Callbacks{};
            std::string SenderName{};
            PluginHandle Target = InvalidPluginHandle;
            {
                std::lock_guard<std::mutex> Lock(Mutex);
                if (ShuttingDown || !IsCallableOwnerLocked(Sender))
                {
                    return false;
                }
                const auto Name = NameForHandleLocked(Sender);
                if (!Name)
                {
                    return false;
                }
                SenderName = Name;
                if (Receiver)
                {
                    const auto Record = FindRecordByNameLocked(Receiver);
                    if (!Record || Record->State != EState::Loaded)
                    {
                        return false;
                    }
                    Target = Record->Handle;
                }
                const auto Iterator = Listeners.find(Sender);
                if (Iterator == Listeners.end())
                {
                    return false;
                }
                for (const auto& Entry : Iterator->second)
                {
                    const auto ListenerRecord = FindRecordByHandleLocked(Entry.Listener);
                    if (!ListenerRecord || ListenerRecord->State != EState::Loaded)
                    {
                        continue;
                    }
                    if (Target == InvalidPluginHandle || Target == Entry.Listener)
                    {
                        Callbacks.push_back(Entry);
                    }
                }
            }

            FMessagingInterface::FMessage Message{
                    SenderName.c_str(), MessageType, DataLength, Data};
            for (const auto& Entry : Callbacks)
            {
                Entry.Callback(&Message);
                if (Target != InvalidPluginHandle)
                {
                    return true;
                }
            }
            return !Callbacks.empty();
        }

        static auto QueryInterfaceThunk(const std::uint32_t Id) -> void*
        {
            const auto Manager = Published.load(std::memory_order_acquire);
            if (!Manager)
            {
                return nullptr;
            }
            if (Id == InterfaceMessaging)
            {
                return &Manager->Messaging;
            }
            if (Id == InterfaceTrampoline)
            {
                if (Manager->TrampolineReady)
                {
                    return &Manager->Trampoline;
                }
                Manager->Emit("query-interface", nullptr, "trampoline-unavailable", Id);
            }
            else
            {
                Manager->Emit("query-interface", nullptr, "unknown-interface", Id);
            }
            return nullptr;
        }

        static auto AllocateFromBranchPoolThunk(const PluginHandle Plugin,
                                                const std::size_t Bytes) -> void*
        {
            const auto Manager = Published.load(std::memory_order_acquire);
            return Manager ? Manager->AllocateTrampoline(true, Plugin, Bytes) : nullptr;
        }

        static auto AllocateFromLocalPoolThunk(const PluginHandle Plugin,
                                               const std::size_t Bytes) -> void*
        {
            const auto Manager = Published.load(std::memory_order_acquire);
            return Manager ? Manager->AllocateTrampoline(false, Plugin, Bytes) : nullptr;
        }

        static auto DataLoadedHookThunk(void* DataHandler) -> void
        {
            const auto OriginalAddress =
                    PublishedDataLoadedOriginal.load(std::memory_order_acquire);
            using FOriginal = void (*)(void*);
            FOriginal Original{};
            static_assert(sizeof(Original) == sizeof(OriginalAddress));
            std::memcpy(&Original, &OriginalAddress, sizeof(Original));
            if (Original)
            {
                Original(DataHandler);
            }
            const auto Manager = Published.load(std::memory_order_acquire);
            if (Manager)
            {
                Manager->DispatchDataLoadedOnce();
            }
        }

        static auto GetPluginHandleThunk() -> PluginHandle
        {
            return CurrentLoadingHandle;
        }

        static auto GetReleaseIndexThunk() -> std::uint32_t
        {
            const auto Manager = Published.load(std::memory_order_acquire);
            return Manager ? Manager->Config.ReleaseIndex : 0;
        }

        static auto GetPluginInfoThunk(const char* Name) -> const FPluginInfo*
        {
            const auto Manager = Published.load(std::memory_order_acquire);
            if (!Manager || !Name)
            {
                return nullptr;
            }
            std::lock_guard<std::mutex> Lock(Manager->Mutex);
            if (!Manager->LoadComplete)
            {
                return nullptr;
            }
            const auto Record = Manager->FindRecordByNameLocked(Name);
            return Record && Record->State == EState::Loaded ? &Record->Info : nullptr;
        }

        static auto GetSaveFolderNameThunk() -> const char*
        {
            const auto Manager = Published.load(std::memory_order_acquire);
            return Manager ? Manager->Config.SaveFolderName.c_str() : "";
        }

        static auto RegisterListenerThunk(const PluginHandle Listener,
                                          const char* Sender,
                                          const FMessagingInterface::FEventCallback Handler) -> bool
        {
            const auto Manager = Published.load(std::memory_order_acquire);
            return Manager && Manager->RegisterListener(Listener, Sender, Handler);
        }

        static auto DispatchThunk(const PluginHandle Sender,
                                  const std::uint32_t MessageType,
                                  void* Data,
                                  const std::uint32_t DataLength,
                                  const char* Receiver) -> bool
        {
            const auto Manager = Published.load(std::memory_order_acquire);
            return Manager && Manager->Dispatch(Sender, MessageType, Data, DataLength, Receiver);
        }

        FPluginManagerConfig Config{};
        FPluginManagerEventSink EventSink{};
        mutable std::mutex Mutex{};
        std::vector<FRecord> Records{};
        std::unordered_map<PluginHandle, std::vector<FListener>> Listeners{};
        FInterface Interface{};
        FMessagingInterface Messaging{};
        FTrampolineInterface Trampoline{};
        FExecutablePool BranchPool{};
        FExecutablePool LocalPool{};
        std::atomic<bool> DataLoadedWasDispatched{};
        std::array<std::uint8_t, 5> OriginalDataLoadedCall{};
        std::array<std::uint8_t, 5> PatchedDataLoadedCall{};
        std::uintptr_t DataLoadedCallSite{};
        bool TrampolineReady{};
        bool DataLoadedHookActive{};
        bool LoadComplete{};
        bool ShuttingDown{};
        bool IsPublished{};
    };

    std::atomic<FPluginManager::FImpl*> FPluginManager::FImpl::Published{};
    std::atomic<std::uintptr_t> FPluginManager::FImpl::PublishedDataLoadedOriginal{};
    thread_local PluginHandle FPluginManager::FImpl::CurrentLoadingHandle = InvalidPluginHandle;

    FPluginManager::FPluginManager(FPluginManagerConfig Config, FPluginManagerEventSink EventSink)
        : m_impl(std::make_unique<FImpl>(std::move(Config), std::move(EventSink)))
    {
    }

    FPluginManager::~FPluginManager()
    {
        Shutdown();
    }

    auto FPluginManager::LoadPaths(const std::vector<std::filesystem::path>& Paths)
            -> FPluginLoadSummary
    {
        FPluginLoadSummary Summary{};
        if (!m_impl)
        {
            return Summary;
        }
        {
            std::lock_guard<std::mutex> Lock(m_impl->Mutex);
            if (!m_impl->Records.empty() || m_impl->ShuttingDown)
            {
                m_impl->Emit("load", nullptr, "already-used");
                return Summary;
            }
            m_impl->Records.reserve(Paths.size());
        }

        auto OrderedPaths = Paths;
        std::sort(OrderedPaths.begin(), OrderedPaths.end());
        std::unordered_map<std::string, bool> Names{};
        PluginHandle NextHandle = 1;
        for (const auto& CandidatePath : OrderedPaths)
        {
            ++Summary.Scanned;
            FImpl::FRecord Record{};
            std::error_code AbsoluteError{};
            Record.Path = std::filesystem::absolute(CandidatePath, AbsoluteError);
            if (AbsoluteError)
            {
                Record.Path = CandidatePath;
            }
            Record.Scan = ScanPlugin(Record.Path);
            if (Record.Scan.Status != EPluginScanStatus::Ok)
            {
                ++Summary.Rejected;
                m_impl->Emit("scan", &Record, PluginScanStatusName(Record.Scan.Status));
                continue;
            }
            const auto FoldedName = FoldAscii(Record.Scan.Name);
            if (Names.find(FoldedName) != Names.end())
            {
                ++Summary.Rejected;
                m_impl->Emit("scan", &Record, "duplicate-plugin-name");
                continue;
            }
            Names.emplace(FoldedName, true);
            Record.Handle = NextHandle++;
            Record.Compatibility = EvaluateCompatibility(
                    Record.Scan,
                    {m_impl->Config.RuntimeVersion,
                     m_impl->Config.EmulatedScriptExtenderVersion,
                     m_impl->Config.AddressLibraryAvailable,
                     m_impl->Config.PreloadTimingAvailable});
            if (Record.Compatibility.Disposition ==
                EPluginCompatibilityDisposition::StructuralBlocker)
            {
                ++Summary.Rejected;
                m_impl->Emit("compatibility", &Record, "structural-blocker");
                continue;
            }
            if (Record.Compatibility.Disposition ==
                EPluginCompatibilityDisposition::UnverifiedAttempt)
            {
                ++Summary.UnverifiedAttempts;
            }
            Record.Info = {PluginInfoVersion, nullptr, Record.Scan.Version.PluginVersion};
            m_impl->Records.push_back(std::move(Record));
            ++Summary.Accepted;
        }

        for (auto& Record : m_impl->Records)
        {
            Record.Info.Name = Record.Scan.Name.c_str();
        }
        FImpl* Expected{};
        if (!FImpl::Published.compare_exchange_strong(
                    Expected, m_impl.get(), std::memory_order_acq_rel))
        {
            m_impl->Emit("load", nullptr, "another-adapter-is-published");
            Summary.LoadFailed += Summary.Accepted;
            return Summary;
        }
        m_impl->IsPublished = true;

        for (auto& Record : m_impl->Records)
        {
            m_impl->Emit("compatibility", &Record,
                         PluginCompatibilityDispositionName(Record.Compatibility.Disposition));
            if (!Record.Scan.HasLoad)
            {
                {
                    std::lock_guard<std::mutex> Lock(m_impl->Mutex);
                    Record.State = FImpl::EState::Failed;
                }
                ++Summary.LoadFailed;
                m_impl->Emit("load", &Record, "load-phase-unavailable");
                continue;
            }
            Record.Module = LoadLibraryExW(
                    Record.Path.c_str(), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!Record.Module)
            {
                {
                    std::lock_guard<std::mutex> Lock(m_impl->Mutex);
                    Record.State = FImpl::EState::Failed;
                }
                ++Summary.LoadFailed;
                m_impl->Emit("load", &Record, "load-library-failed", GetLastError());
                continue;
            }
            const auto Procedure = GetProcAddress(Record.Module, "OBSEPlugin_Load");
            FPluginLoad Load{};
            static_assert(sizeof(Procedure) == sizeof(Load));
            std::memcpy(&Load, &Procedure, sizeof(Load));
            if (!Load)
            {
                FreeLibrary(Record.Module);
                Record.Module = nullptr;
                {
                    std::lock_guard<std::mutex> Lock(m_impl->Mutex);
                    Record.State = FImpl::EState::Failed;
                }
                ++Summary.LoadFailed;
                m_impl->Emit("load", &Record, "load-export-unavailable");
                continue;
            }

            {
                std::lock_guard<std::mutex> Lock(m_impl->Mutex);
                Record.State = FImpl::EState::Loading;
            }
            FImpl::CurrentLoadingHandle = Record.Handle;
            const auto Loaded = Load(&m_impl->Interface);
            FImpl::CurrentLoadingHandle = InvalidPluginHandle;
            if (!Loaded)
            {
                FreeLibrary(Record.Module);
                Record.Module = nullptr;
                {
                    std::lock_guard<std::mutex> Lock(m_impl->Mutex);
                    Record.State = FImpl::EState::Failed;
                }
                ++Summary.LoadFailed;
                m_impl->Emit("load", &Record, "plugin-returned-false");
                continue;
            }
            {
                std::lock_guard<std::mutex> Lock(m_impl->Mutex);
                Record.State = FImpl::EState::Loaded;
            }
            ++Summary.Loaded;
            m_impl->Emit("load", &Record, "loaded");
        }
        return Summary;
    }

    auto FPluginManager::DispatchPostLoad() -> bool
    {
        if (!m_impl)
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> Lock(m_impl->Mutex);
            if (m_impl->ShuttingDown)
            {
                return false;
            }
            m_impl->LoadComplete = true;
        }
        const auto First = m_impl->Dispatch(CorePluginHandle, MessagePostLoad, nullptr, 0, nullptr);
        const auto Second = m_impl->Dispatch(
                CorePluginHandle, MessagePostPostLoad, nullptr, 0, nullptr);
        m_impl->Emit("lifecycle", nullptr,
                     First || Second ? "post-load-dispatched" : "post-load-no-listeners");
        return First || Second;
    }

    auto FPluginManager::DispatchDataLoaded() -> bool
    {
        if (!m_impl)
        {
            return false;
        }
        return m_impl->DispatchDataLoadedOnce();
    }

    auto FPluginManager::IsDataLoadedHookAnchor(const std::uintptr_t CallSite,
                                                const std::uintptr_t ExpectedTarget) -> bool
    {
        return ReadExpectedDirectCall(CallSite, ExpectedTarget);
    }

    auto FPluginManager::InstallDataLoadedHook(const std::uintptr_t CallSite,
                                               const std::uintptr_t ExpectedTarget) -> bool
    {
        if (!m_impl)
        {
            return false;
        }
        const auto Installed = m_impl->InstallDataLoadedHook(CallSite, ExpectedTarget);
        m_impl->Emit("data-loaded-hook", nullptr,
                     Installed ? "installed" : "install-failed");
        return Installed;
    }

    auto FPluginManager::RemoveDataLoadedHook() -> bool
    {
        if (!m_impl)
        {
            return false;
        }
        const auto Removed = m_impl->RemoveDataLoadedHook();
        m_impl->Emit("data-loaded-hook", nullptr,
                     Removed ? "removed" : "remove-anchor-mismatch");
        return Removed;
    }

    auto FPluginManager::Shutdown() -> void
    {
        if (!m_impl)
        {
            return;
        }
        RemoveDataLoadedHook();
        if (m_impl->IsPublished)
        {
            auto Expected = m_impl.get();
            FImpl::Published.compare_exchange_strong(
                    Expected, nullptr, std::memory_order_acq_rel);
            m_impl->IsPublished = false;
        }
        std::vector<HMODULE> Modules{};
        {
            std::lock_guard<std::mutex> Lock(m_impl->Mutex);
            if (m_impl->ShuttingDown)
            {
                return;
            }
            m_impl->ShuttingDown = true;
            m_impl->Listeners.clear();
            for (auto Iterator = m_impl->Records.rbegin(); Iterator != m_impl->Records.rend();
                 ++Iterator)
            {
                if (Iterator->Module)
                {
                    Modules.push_back(Iterator->Module);
                    Iterator->Module = nullptr;
                }
                Iterator->State = FImpl::EState::Failed;
            }
        }
        for (const auto Module : Modules)
        {
            FreeLibrary(Module);
        }
        m_impl->BranchPool.Release();
        m_impl->LocalPool.Release();
        m_impl->TrampolineReady = false;
        m_impl->Emit("shutdown", nullptr, "complete");
    }

    auto FPluginManager::LoadedCount() const -> std::size_t
    {
        if (!m_impl)
        {
            return 0;
        }
        std::lock_guard<std::mutex> Lock(m_impl->Mutex);
        return static_cast<std::size_t>(std::count_if(
                m_impl->Records.begin(), m_impl->Records.end(),
                [](const FImpl::FRecord& Record) {
                    return Record.State == FImpl::EState::Loaded;
                }));
    }

    auto FPluginManager::GetInfo(const char* Name) const -> const FPluginInfo*
    {
        if (!m_impl || !Name)
        {
            return nullptr;
        }
        std::lock_guard<std::mutex> Lock(m_impl->Mutex);
        if (!m_impl->LoadComplete)
        {
            return nullptr;
        }
        const auto Record = m_impl->FindRecordByNameLocked(Name);
        return Record && Record->State == FImpl::EState::Loaded ? &Record->Info : nullptr;
    }

    auto FPluginManager::TrampolineAvailable() const -> bool
    {
        if (!m_impl)
        {
            return false;
        }
        std::lock_guard<std::mutex> Lock(m_impl->Mutex);
        return m_impl->TrampolineReady && !m_impl->ShuttingDown;
    }

    auto FPluginManager::DataLoadedHookInstalled() const -> bool
    {
        if (!m_impl)
        {
            return false;
        }
        std::lock_guard<std::mutex> Lock(m_impl->Mutex);
        return m_impl->DataLoadedHookActive;
    }

    auto FPluginManager::DataLoadedDispatched() const -> bool
    {
        return m_impl &&
               m_impl->DataLoadedWasDispatched.load(std::memory_order_acquire);
    }
} // namespace RC::UNBSE::OBSE64
