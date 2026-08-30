#include <OBSE64PluginManager.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <Windows.h>
#include <Psapi.h>
#include <winver.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UNBSEAddonHostV1.h>
#include <UNBSERelocationV1.h>
#include <UNBSERuntimeInfoV1.h>

using namespace RC;

namespace
{
    constexpr auto AddonId = "unbse.obse64-interop";
    constexpr auto AddonVersion = "0.1.0";
    constexpr auto SupportedRuntimeVersion = UNBSE::OBSE64::PackRuntimeVersion(1, 512, 105, 0);
    constexpr std::uint64_t DataLoadedCallRva = 0x065D2A42u;
    constexpr std::uint64_t DataLoadedOriginalTargetRva = 0x0662E470u;

    struct FUNBSEConnection
    {
        UNBSEAddonHostV1 Host{};
        UNBSERelocationV1 Relocation{};
        std::uint32_t OwnerHandle{};
    };

    auto LogJson(const std::string& Json) -> void
    {
        Output::send(STR("[UNBSE.OBSE64Interop] {}\n"), ensure_str(Json));
    }

    auto LogStatus(const char* Event, const char* Status, const std::uint32_t Detail = 0) -> void
    {
        Output::send(
                STR("[UNBSE.OBSE64Interop] {{\"schema\":\"UNBSE.OBSE64Interop\","
                    "\"schemaVersion\":1,\"event\":\"{}\",\"status\":\"{}\","
                    "\"detail\":{}}}\n"),
                 ensure_str(Event), ensure_str(Status), Detail);
    }

    auto FindLegacyOBSE64Runtime() -> std::wstring
    {
        std::array<HMODULE, 1024> Modules{};
        DWORD BytesNeeded{};
        if (!EnumProcessModules(GetCurrentProcess(), Modules.data(),
                                static_cast<DWORD>(sizeof(Modules)), &BytesNeeded))
        {
            return {};
        }
        const auto Count = std::min<std::size_t>(
                Modules.size(), BytesNeeded / sizeof(HMODULE));
        for (std::size_t Index = 0; Index < Count; ++Index)
        {
            std::array<wchar_t, MAX_PATH> Name{};
            if (GetModuleBaseNameW(GetCurrentProcess(), Modules[Index], Name.data(),
                                   static_cast<DWORD>(Name.size())) == 0)
            {
                continue;
            }
            auto Lower = std::wstring{Name.data()};
            std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                           [](const wchar_t Character) {
                               return static_cast<wchar_t>(std::towlower(Character));
                           });
            const auto IsRuntime =
                    Lower == L"obse64_steam_loader.dll" ||
                    (Lower.starts_with(L"obse64_") && Lower.ends_with(L".dll"));
            if (IsRuntime)
            {
                return Name.data();
            }
        }
        return {};
    }

    auto LogRuntimeConflict(const std::wstring& Module) -> void
    {
        Output::send(
                STR("[UNBSE.OBSE64Interop] {{\"schema\":\"UNBSE.OBSE64Interop\","
                    "\"schemaVersion\":1,\"event\":\"runtime-conflict\","
                    "\"status\":\"legacy-runtime-loaded-stand-down\","
                    "\"module\":\"{}\"}}\n"),
                ensure_str(Module));
    }

    template <typename Function>
    auto FindExport(const char* Name) -> Function
    {
        std::array<HMODULE, 1024> Modules{};
        DWORD BytesNeeded{};
        if (!EnumProcessModules(GetCurrentProcess(), Modules.data(),
                                static_cast<DWORD>(sizeof(Modules)), &BytesNeeded))
        {
            return nullptr;
        }
        const auto Count = std::min<std::size_t>(
                Modules.size(), BytesNeeded / sizeof(HMODULE));
        for (std::size_t Index = 0; Index < Count; ++Index)
        {
            const auto Address = GetProcAddress(Modules[Index], Name);
            if (!Address)
            {
                continue;
            }
            static_assert(sizeof(Address) == sizeof(Function));
            Function Result{};
            std::memcpy(&Result, &Address, sizeof(Result));
            return Result;
        }
        return nullptr;
    }

    auto RegisterAddon() -> FUNBSEConnection
    {
        FUNBSEConnection Connection{};
        const auto Query = FindExport<UNBSEQueryAddonHostV1Function>("UNBSE_QueryAddonHostV1");
        if (!Query)
        {
            LogStatus("registration", "host-not-found-unverified-attempt");
            return Connection;
        }
        Connection.Host.structSize = sizeof(Connection.Host);
        if (!Query(UNBSE_ADDON_HOST_ABI_VERSION, &Connection.Host))
        {
            LogStatus("registration", "host-abi-unavailable-unverified-attempt");
            Connection.Host = {};
            return Connection;
        }

        UNBSEAddonDescriptorV1 Descriptor{};
        Descriptor.structSize = sizeof(Descriptor);
        Descriptor.apiVersion = UNBSE_ADDON_HOST_ABI_VERSION;
        Descriptor.declaredEffects = UNBSE_ADDON_EFFECT_RUNTIME_READ |
                                     UNBSE_ADDON_EFFECT_RUNTIME_WRITE |
                                     UNBSE_ADDON_EFFECT_FILE_IO;
        Descriptor.requiredHostCapabilities = UNBSE_ADDON_HOST_CAPABILITY_RUNTIME_INFO_V1 |
                                              UNBSE_ADDON_HOST_CAPABILITY_RELOCATION_V1;
        std::memcpy(Descriptor.addonId, AddonId, sizeof(AddonId));
        std::memcpy(Descriptor.addonVersion, AddonVersion, sizeof(AddonVersion));

        UNBSEAddonRegistrationV1 Registration{};
        Registration.structSize = sizeof(Registration);
        Registration.apiVersion = UNBSE_ADDON_HOST_ABI_VERSION;
        const auto Result = Connection.Host.registerAddon(&Descriptor, &Registration);
        if (Result != UNBSE_ADDON_RESULT_OK)
        {
            LogStatus("registration", "registration-failed-unverified-attempt", Result);
            Connection.Host = {};
            return Connection;
        }
        Connection.OwnerHandle = Registration.ownerHandle;
        const auto QueryRelocation =
                FindExport<UNBSEQueryRelocationV1Function>("UNBSE_QueryRelocationV1");
        Connection.Relocation.structSize = sizeof(Connection.Relocation);
        if (!QueryRelocation ||
            !QueryRelocation(UNBSE_RELOCATION_ABI_VERSION, &Connection.Relocation) ||
            !Connection.Relocation.resolve)
        {
            Connection.Relocation = {};
            LogStatus("registration", "relocation-v1-unavailable-unverified-attempt");
        }
        LogStatus("registration",
                  Registration.compatibility == UNBSE_ADDON_COMPATIBILITY_VERIFIED
                          ? "verified"
                          : "unverified-attempt",
                  Registration.missingHostCapabilities);
        return Connection;
    }

    auto ResolveExecutableRva(const FUNBSEConnection& Connection,
                              const std::uint64_t RelativeAddress,
                              const std::uint64_t MinimumBytes) -> std::uintptr_t
    {
        if (Connection.OwnerHandle == 0 || !Connection.Relocation.resolve)
        {
            return 0;
        }
        UNBSERelocationRequestV1 Request{};
        Request.structSize = sizeof(Request);
        Request.apiVersion = UNBSE_RELOCATION_ABI_VERSION;
        Request.ownerHandle = Connection.OwnerHandle;
        Request.module = UNBSE_RELOCATION_MODULE_EXECUTABLE;
        Request.relativeAddress = RelativeAddress;
        Request.minimumBytes = MinimumBytes;
        UNBSERelocationResultV1 Result{};
        Result.structSize = sizeof(Result);
        Result.apiVersion = UNBSE_RELOCATION_ABI_VERSION;
        const auto Status = Connection.Relocation.resolve(&Request, &Result);
        if (Status != UNBSE_RELOCATION_RESULT_OK || Result.relativeAddress != RelativeAddress)
        {
            LogStatus("relocation", "resolve-failed", Status);
            return 0;
        }
        return static_cast<std::uintptr_t>(Result.absoluteAddress);
    }

    auto GetExecutablePath() -> std::filesystem::path
    {
        std::vector<wchar_t> Buffer(32768);
        const auto Length = GetModuleFileNameW(
                nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
        return Length > 0 && Length < Buffer.size()
                       ? std::filesystem::path{std::wstring{Buffer.data(), Length}}
                       : std::filesystem::path{};
    }

    auto GetExecutableFileVersion(const std::filesystem::path& Executable) -> std::uint32_t
    {
        DWORD Ignored{};
        const auto Bytes = GetFileVersionInfoSizeW(Executable.c_str(), &Ignored);
        if (Bytes == 0)
        {
            return 0;
        }
        std::vector<std::uint8_t> Data(Bytes);
        if (!GetFileVersionInfoW(Executable.c_str(), 0, Bytes, Data.data()))
        {
            return 0;
        }
        VS_FIXEDFILEINFO* Info{};
        UINT InfoBytes{};
        if (!VerQueryValueW(Data.data(), L"\\", reinterpret_cast<void**>(&Info), &InfoBytes) ||
            !Info || InfoBytes < sizeof(VS_FIXEDFILEINFO) ||
            Info->dwSignature != 0xFEEF04BD)
        {
            return 0;
        }
        return UNBSE::OBSE64::PackRuntimeVersion(
                HIWORD(Info->dwFileVersionMS), LOWORD(Info->dwFileVersionMS),
                HIWORD(Info->dwFileVersionLS), LOWORD(Info->dwFileVersionLS));
    }

    auto GetPluginPaths(const std::filesystem::path& GameRoot) -> std::vector<std::filesystem::path>
    {
        std::vector<std::filesystem::path> Paths{};
        const auto Directory = GameRoot / L"OBSE" / L"Plugins";
        std::error_code Error{};
        for (std::filesystem::directory_iterator Iterator{Directory, Error}, End;
             !Error && Iterator != End; Iterator.increment(Error))
        {
            if (!Iterator->is_regular_file(Error))
            {
                continue;
            }
            auto Extension = Iterator->path().extension().wstring();
            std::transform(Extension.begin(), Extension.end(), Extension.begin(), ::towlower);
            if (Extension == L".dll")
            {
                Paths.push_back(Iterator->path());
            }
        }
        if (Error && Error != std::errc::no_such_file_or_directory)
        {
            LogStatus("scan-directory", "enumeration-failed", Error.value());
        }
        return Paths;
    }

    auto HasAddressLibrary(const std::filesystem::path& GameRoot,
                           const std::uint32_t RuntimeVersion) -> bool
    {
        const auto Major = (RuntimeVersion >> 28u) & 0xFu;
        const auto Minor = (RuntimeVersion >> 16u) & 0xFFFu;
        const auto Build = (RuntimeVersion >> 4u) & 0xFFFu;
        const auto Name = L"versionlib-" + std::to_wstring(Major) + L"-" +
                          std::to_wstring(Minor) + L"-" + std::to_wstring(Build) + L"-0.bin";
        return std::filesystem::is_regular_file(GameRoot / L"OBSE" / L"Plugins" / Name);
    }
} // namespace

class UNBSEOBSE64Interop final : public CppUserModBase
{
  public:
    UNBSEOBSE64Interop()
    {
        ModName = STR("UNBSEOBSE64Interop");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Clean-room OBSE64 plugin ABI interoperability add-on for UNBSE");
        ModAuthors = STR("Unblivion Project");

        if (const auto LegacyRuntime = FindLegacyOBSE64Runtime(); !LegacyRuntime.empty())
        {
            m_runtime_conflict = true;
            LogRuntimeConflict(LegacyRuntime);
            return;
        }

        m_connection = RegisterAddon();
        const auto Executable = GetExecutablePath();
        if (Executable.empty())
        {
            LogStatus("startup", "executable-path-unavailable");
            return;
        }
        const auto ExecutableFileVersion = GetExecutableFileVersion(Executable);
        if (ExecutableFileVersion == 0)
        {
            LogStatus("startup", "executable-file-version-unavailable");
        }
        const auto GameRoot = Executable.parent_path();
        const auto CallSite = ResolveExecutableRva(m_connection, DataLoadedCallRva, 5);
        const auto ExpectedTarget = ResolveExecutableRva(
                m_connection, DataLoadedOriginalTargetRva, 1);
        const auto ExactRuntimeAnchor =
                CallSite != 0 && ExpectedTarget != 0 &&
                UNBSE::OBSE64::FPluginManager::IsDataLoadedHookAnchor(
                        CallSite, ExpectedTarget);
        const auto RuntimeVersion = ExactRuntimeAnchor ? SupportedRuntimeVersion : 0u;
        LogStatus("runtime-detection",
                  ExactRuntimeAnchor ? "exact-call-anchor-verified"
                                     : "exact-call-anchor-unverified",
                  ExecutableFileVersion);
        m_manager = std::make_unique<UNBSE::OBSE64::FPluginManager>(
                UNBSE::OBSE64::FPluginManagerConfig{
                        RuntimeVersion,
                        UNBSE::OBSE64::PackRuntimeVersion(0, 2, 2),
                        1,
                        "Oblivion Remastered",
                        HasAddressLibrary(GameRoot, RuntimeVersion),
                        false},
                &LogJson);
        const auto Summary = m_manager->LoadPaths(GetPluginPaths(GameRoot));
        if (!ExactRuntimeAnchor)
        {
            LogStatus("data-loaded-hook", "runtime-anchor-unverified-withheld",
                      ExecutableFileVersion);
        }
        else
        {
            m_data_loaded_hook_armed = m_manager->InstallDataLoadedHook(
                    CallSite, ExpectedTarget);
            LogStatus("data-loaded-hook",
                      m_data_loaded_hook_armed ? "exact-call-anchor-installed"
                                               : "exact-call-anchor-unavailable");
        }
        m_manager->DispatchPostLoad();
        Output::send(
                STR("[UNBSE.OBSE64Interop] {{\"schema\":\"UNBSE.OBSE64InteropSummary\","
                    "\"schemaVersion\":1,\"runtimeVersion\":{},\"scanned\":{},"
                    "\"accepted\":{},\"loaded\":{},\"rejected\":{},"
                    "\"loadFailed\":{},\"unverifiedAttempts\":{},"
                    "\"trampoline\":{},\"preload\":false,"
                    "\"dataLoaded\":{}}}\n"),
                RuntimeVersion, Summary.Scanned, Summary.Accepted, Summary.Loaded,
                Summary.Rejected, Summary.LoadFailed, Summary.UnverifiedAttempts,
                ensure_str(m_manager->TrampolineAvailable() ? "true" : "false"),
                ensure_str(m_data_loaded_hook_armed ? "true" : "false"));
    }

    ~UNBSEOBSE64Interop() override
    {
        m_manager.reset();
        if (m_connection.OwnerHandle != 0 && m_connection.Host.retireAddon)
        {
            const auto Result = m_connection.Host.retireAddon(m_connection.OwnerHandle, 50);
            LogStatus("retirement", Result == UNBSE_ADDON_RESULT_OK ? "retired" : "failed",
                      Result);
        }
    }

    auto on_unreal_init() -> void override
    {
        if (m_runtime_conflict)
        {
            LogStatus("lifecycle", "runtime-conflict-stand-down");
            return;
        }
        LogStatus("lifecycle",
                  m_manager && m_manager->DataLoadedDispatched()
                          ? "data-loaded-exact-call-observed"
                          : (m_data_loaded_hook_armed ? "data-loaded-exact-call-armed"
                                                     : "data-loaded-exact-call-unavailable"));
    }

  private:
    FUNBSEConnection m_connection{};
    std::unique_ptr<UNBSE::OBSE64::FPluginManager> m_manager{};
    bool m_data_loaded_hook_armed{};
    bool m_runtime_conflict{};
};

#define UNBSE_OBSE64_INTEROP_API __declspec(dllexport)
extern "C"
{
    UNBSE_OBSE64_INTEROP_API CppUserModBase* start_mod()
    {
        return new UNBSEOBSE64Interop();
    }

    UNBSE_OBSE64_INTEROP_API void uninstall_mod(CppUserModBase* Mod)
    {
        delete Mod;
    }
}
