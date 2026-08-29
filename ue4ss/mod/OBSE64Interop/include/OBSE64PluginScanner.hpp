#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace RC::UNBSE::OBSE64
{
    constexpr std::uint32_t PluginVersionDataVersion = 1;
    constexpr std::uint32_t AddressIndependenceSignatures = 1u << 0;
    constexpr std::uint32_t AddressIndependenceAddressLibrary = 1u << 1;
    constexpr std::uint32_t StructureIndependenceNoStructs = 1u << 0;
    constexpr std::uint32_t StructureIndependenceInitialLayout = 1u << 1;

    struct FPluginVersionDataV1
    {
        std::uint32_t DataVersion{};
        std::uint32_t PluginVersion{};
        std::array<char, 256> Name{};
        std::array<char, 256> Author{};
        std::uint32_t AddressIndependence{};
        std::uint32_t StructureIndependence{};
        std::array<std::uint32_t, 16> CompatibleVersions{};
        std::uint32_t ScriptExtenderVersionRequired{};
        std::uint32_t ReservedNonBreaking{};
        std::uint32_t ReservedBreaking{};
        std::array<std::uint8_t, 512> Reserved{};
    };

    static_assert(sizeof(FPluginVersionDataV1) == 1116,
                  "OBSE64 plugin version declaration layout drift");

    enum class EPluginScanStatus : std::uint32_t
    {
        Ok = 0,
        FileReadFailed,
        FileTooLarge,
        NotPortableExecutable,
        WrongArchitecture,
        MalformedPortableExecutable,
        MissingVersionExport,
        VersionExportOutOfBounds,
        UnsupportedVersionDeclaration,
        MissingPluginName
    };

    struct FPluginScanResult
    {
        EPluginScanStatus Status{EPluginScanStatus::FileReadFailed};
        std::uint16_t Machine{};
        bool HasLoad{};
        bool HasPreload{};
        FPluginVersionDataV1 Version{};
        std::string Name{};
        std::string Author{};
    };

    constexpr auto PackRuntimeVersion(const std::uint32_t Major,
                                      const std::uint32_t Minor,
                                      const std::uint32_t Build,
                                      const std::uint32_t Sub = 0) -> std::uint32_t
    {
        return ((Major & 0xFu) << 28u) | ((Minor & 0xFFFu) << 16u) |
               ((Build & 0xFFFu) << 4u) | (Sub & 0xFu);
    }

    enum class EPluginCompatibilityDisposition : std::uint32_t
    {
        Verified = 0,
        UnverifiedAttempt,
        StructuralBlocker
    };

    enum EPluginCompatibilityReason : std::uint32_t
    {
        CompatibilityReasonNone = 0,
        CompatibilityReasonRuntimeMismatch = 1u << 0,
        CompatibilityReasonScriptExtenderTooOld = 1u << 1,
        CompatibilityReasonAddressLibraryUnavailable = 1u << 2,
        CompatibilityReasonPreloadTimingUnavailable = 1u << 3,
        CompatibilityReasonNoLoadPhase = 1u << 4,
        CompatibilityReasonStructuralScanFailure = 1u << 5
    };

    struct FPluginCompatibilityContext
    {
        std::uint32_t RuntimeVersion{};
        std::uint32_t EmulatedScriptExtenderVersion{};
        bool AddressLibraryAvailable{};
        bool PreloadTimingAvailable{};
    };

    struct FPluginCompatibilityResult
    {
        EPluginCompatibilityDisposition Disposition{
                EPluginCompatibilityDisposition::StructuralBlocker};
        std::uint32_t Reasons{CompatibilityReasonStructuralScanFailure};
    };

    auto ScanPlugin(const std::filesystem::path& Path) -> FPluginScanResult;
    auto EvaluateCompatibility(const FPluginScanResult& Plugin,
                               const FPluginCompatibilityContext& Context)
            -> FPluginCompatibilityResult;
    auto PluginScanStatusName(EPluginScanStatus Status) -> const char*;
    auto PluginCompatibilityDispositionName(EPluginCompatibilityDisposition Disposition)
            -> const char*;
} // namespace RC::UNBSE::OBSE64
