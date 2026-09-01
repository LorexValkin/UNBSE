#include <OBSE64PluginScanner.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>

#include <Windows.h>

namespace RC::UNBSE::OBSE64
{
    namespace
    {
        constexpr std::uint64_t MaximumPluginBytes = 512ull * 1024ull * 1024ull;
        constexpr std::uint32_t MaximumExportCount = 65536;

        template <typename T>
        auto ReadObject(const std::vector<std::uint8_t>& Bytes, const std::uint64_t Offset)
                -> std::optional<T>
        {
            if (Offset > Bytes.size() || sizeof(T) > Bytes.size() - Offset)
            {
                return std::nullopt;
            }
            T Value{};
            std::memcpy(&Value, Bytes.data() + static_cast<std::size_t>(Offset), sizeof(T));
            return Value;
        }

        struct FPeImage
        {
            const std::vector<std::uint8_t>& Bytes;
            IMAGE_FILE_HEADER FileHeader{};
            IMAGE_OPTIONAL_HEADER64 OptionalHeader{};
            std::vector<IMAGE_SECTION_HEADER> Sections{};

            auto RvaToFileOffset(const std::uint32_t Rva, const std::uint64_t RequiredBytes) const
                    -> std::optional<std::uint64_t>
            {
                if (RequiredBytes == 0)
                {
                    return std::nullopt;
                }
                if (Rva < OptionalHeader.SizeOfHeaders && Rva <= Bytes.size() &&
                    RequiredBytes <= Bytes.size() - Rva)
                {
                    return Rva;
                }
                for (const auto& Section : Sections)
                {
                    const auto Start = static_cast<std::uint64_t>(Section.VirtualAddress);
                    const auto VirtualSpan = static_cast<std::uint64_t>(
                            std::max(Section.Misc.VirtualSize, Section.SizeOfRawData));
                    if (Rva < Start || static_cast<std::uint64_t>(Rva) - Start >= VirtualSpan)
                    {
                        continue;
                    }
                    const auto Delta = static_cast<std::uint64_t>(Rva) - Start;
                    if (Delta > Section.SizeOfRawData ||
                        RequiredBytes > static_cast<std::uint64_t>(Section.SizeOfRawData) - Delta)
                    {
                        return std::nullopt;
                    }
                    const auto Raw = static_cast<std::uint64_t>(Section.PointerToRawData) + Delta;
                    if (Raw > Bytes.size() || RequiredBytes > Bytes.size() - Raw)
                    {
                        return std::nullopt;
                    }
                    return Raw;
                }
                return std::nullopt;
            }
        };

        auto ParseImage(const std::vector<std::uint8_t>& Bytes, EPluginScanStatus& Status)
                -> std::optional<FPeImage>
        {
            const auto Dos = ReadObject<IMAGE_DOS_HEADER>(Bytes, 0);
            if (!Dos || Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0)
            {
                Status = EPluginScanStatus::NotPortableExecutable;
                return std::nullopt;
            }
            const auto NtOffset = static_cast<std::uint64_t>(Dos->e_lfanew);
            const auto Signature = ReadObject<std::uint32_t>(Bytes, NtOffset);
            const auto FileHeader = ReadObject<IMAGE_FILE_HEADER>(Bytes, NtOffset + sizeof(std::uint32_t));
            if (!Signature || *Signature != IMAGE_NT_SIGNATURE || !FileHeader)
            {
                Status = EPluginScanStatus::MalformedPortableExecutable;
                return std::nullopt;
            }
            if (FileHeader->Machine != IMAGE_FILE_MACHINE_AMD64)
            {
                Status = EPluginScanStatus::WrongArchitecture;
                return std::nullopt;
            }
            if (FileHeader->NumberOfSections == 0 || FileHeader->NumberOfSections > 96 ||
                FileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
            {
                Status = EPluginScanStatus::MalformedPortableExecutable;
                return std::nullopt;
            }
            const auto OptionalOffset = NtOffset + sizeof(std::uint32_t) + sizeof(IMAGE_FILE_HEADER);
            const auto OptionalHeader = ReadObject<IMAGE_OPTIONAL_HEADER64>(Bytes, OptionalOffset);
            if (!OptionalHeader || OptionalHeader->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                OptionalHeader->NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            {
                Status = EPluginScanStatus::MalformedPortableExecutable;
                return std::nullopt;
            }
            const auto SectionsOffset = OptionalOffset + FileHeader->SizeOfOptionalHeader;
            const auto SectionsBytes = static_cast<std::uint64_t>(FileHeader->NumberOfSections) *
                                       sizeof(IMAGE_SECTION_HEADER);
            if (SectionsOffset > Bytes.size() || SectionsBytes > Bytes.size() - SectionsOffset)
            {
                Status = EPluginScanStatus::MalformedPortableExecutable;
                return std::nullopt;
            }
            std::vector<IMAGE_SECTION_HEADER> Sections(FileHeader->NumberOfSections);
            std::memcpy(Sections.data(), Bytes.data() + static_cast<std::size_t>(SectionsOffset),
                        static_cast<std::size_t>(SectionsBytes));
            return FPeImage{Bytes, *FileHeader, *OptionalHeader, std::move(Sections)};
        }

        auto ReadCString(const FPeImage& Image, const std::uint32_t Rva)
                -> std::optional<std::string>
        {
            const auto Offset = Image.RvaToFileOffset(Rva, 1);
            if (!Offset)
            {
                return std::nullopt;
            }
            const auto Available = Image.Bytes.size() - *Offset;
            const auto Limit = std::min<std::uint64_t>(Available, 1024);
            const auto Start = reinterpret_cast<const char*>(
                    Image.Bytes.data() + static_cast<std::size_t>(*Offset));
            const auto Terminator = static_cast<const char*>(std::memchr(Start, '\0', Limit));
            if (!Terminator)
            {
                return std::nullopt;
            }
            return std::string{Start, static_cast<std::size_t>(Terminator - Start)};
        }

        struct FExportTable
        {
            IMAGE_EXPORT_DIRECTORY Directory{};
            std::uint32_t DirectoryRva{};
            std::uint32_t DirectorySize{};
        };

        auto GetExportTable(const FPeImage& Image) -> std::optional<FExportTable>
        {
            const auto& Entry = Image.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (Entry.VirtualAddress == 0 || Entry.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
            {
                return std::nullopt;
            }
            const auto Offset = Image.RvaToFileOffset(Entry.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
            if (!Offset)
            {
                return std::nullopt;
            }
            const auto Directory = ReadObject<IMAGE_EXPORT_DIRECTORY>(Image.Bytes, *Offset);
            if (!Directory || Directory->NumberOfNames > MaximumExportCount ||
                Directory->NumberOfFunctions > MaximumExportCount)
            {
                return std::nullopt;
            }
            return FExportTable{*Directory, Entry.VirtualAddress, Entry.Size};
        }

        auto FindExportRva(const FPeImage& Image,
                           const FExportTable& Exports,
                           const char* Wanted) -> std::optional<std::uint32_t>
        {
            const auto NamesBytes = static_cast<std::uint64_t>(Exports.Directory.NumberOfNames) *
                                    sizeof(std::uint32_t);
            const auto OrdinalsBytes = static_cast<std::uint64_t>(Exports.Directory.NumberOfNames) *
                                       sizeof(std::uint16_t);
            const auto FunctionsBytes = static_cast<std::uint64_t>(Exports.Directory.NumberOfFunctions) *
                                        sizeof(std::uint32_t);
            const auto NamesOffset = Image.RvaToFileOffset(Exports.Directory.AddressOfNames, NamesBytes);
            const auto OrdinalsOffset = Image.RvaToFileOffset(
                    Exports.Directory.AddressOfNameOrdinals, OrdinalsBytes);
            const auto FunctionsOffset = Image.RvaToFileOffset(
                    Exports.Directory.AddressOfFunctions, FunctionsBytes);
            if (!NamesOffset || !OrdinalsOffset || !FunctionsOffset)
            {
                return std::nullopt;
            }
            for (std::uint32_t Index = 0; Index < Exports.Directory.NumberOfNames; ++Index)
            {
                const auto NameRva = ReadObject<std::uint32_t>(
                        Image.Bytes, *NamesOffset + static_cast<std::uint64_t>(Index) * sizeof(std::uint32_t));
                const auto Ordinal = ReadObject<std::uint16_t>(
                        Image.Bytes, *OrdinalsOffset + static_cast<std::uint64_t>(Index) * sizeof(std::uint16_t));
                if (!NameRva || !Ordinal || *Ordinal >= Exports.Directory.NumberOfFunctions)
                {
                    return std::nullopt;
                }
                const auto Name = ReadCString(Image, *NameRva);
                if (!Name)
                {
                    return std::nullopt;
                }
                if (*Name != Wanted)
                {
                    continue;
                }
                const auto FunctionRva = ReadObject<std::uint32_t>(
                        Image.Bytes,
                        *FunctionsOffset + static_cast<std::uint64_t>(*Ordinal) * sizeof(std::uint32_t));
                if (!FunctionRva || *FunctionRva == 0)
                {
                    return std::nullopt;
                }
                const auto ExportEnd = static_cast<std::uint64_t>(Exports.DirectoryRva) +
                                       Exports.DirectorySize;
                if (*FunctionRva >= Exports.DirectoryRva && *FunctionRva < ExportEnd)
                {
                    return std::nullopt;
                }
                return *FunctionRva;
            }
            return std::nullopt;
        }

        template <std::size_t Size>
        auto BoundedString(const std::array<char, Size>& Value) -> std::optional<std::string>
        {
            const auto End = std::find(Value.begin(), Value.end(), '\0');
            if (End == Value.end())
            {
                return std::nullopt;
            }
            return std::string{Value.begin(), End};
        }
    } // namespace

    auto ScanPlugin(const std::filesystem::path& Path) -> FPluginScanResult
    {
        FPluginScanResult Result{};
        std::ifstream Input{Path, std::ios::binary | std::ios::ate};
        if (!Input)
        {
            Result.Status = EPluginScanStatus::FileReadFailed;
            return Result;
        }
        const auto End = Input.tellg();
        if (End <= 0)
        {
            Result.Status = EPluginScanStatus::FileReadFailed;
            return Result;
        }
        const auto Size = static_cast<std::uint64_t>(End);
        if (Size > MaximumPluginBytes || Size > std::numeric_limits<std::size_t>::max())
        {
            Result.Status = EPluginScanStatus::FileTooLarge;
            return Result;
        }
        std::vector<std::uint8_t> Bytes(static_cast<std::size_t>(Size));
        Input.seekg(0, std::ios::beg);
        Input.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
        if (!Input)
        {
            Result.Status = EPluginScanStatus::FileReadFailed;
            return Result;
        }

        auto ParseStatus = EPluginScanStatus::MalformedPortableExecutable;
        const auto Image = ParseImage(Bytes, ParseStatus);
        if (!Image)
        {
            Result.Status = ParseStatus;
            return Result;
        }
        Result.Machine = Image->FileHeader.Machine;
        const auto Exports = GetExportTable(*Image);
        if (!Exports)
        {
            Result.Status = EPluginScanStatus::MissingVersionExport;
            return Result;
        }
        Result.HasLoad = FindExportRva(*Image, *Exports, "OBSEPlugin_Load").has_value();
        Result.HasPreload = FindExportRva(*Image, *Exports, "OBSEPlugin_Preload").has_value();
        const auto VersionRva = FindExportRva(*Image, *Exports, "OBSEPlugin_Version");
        if (!VersionRva)
        {
            Result.Status = EPluginScanStatus::MissingVersionExport;
            return Result;
        }
        const auto VersionOffset = Image->RvaToFileOffset(*VersionRva, sizeof(FPluginVersionDataV1));
        if (!VersionOffset)
        {
            Result.Status = EPluginScanStatus::VersionExportOutOfBounds;
            return Result;
        }
        const auto Version = ReadObject<FPluginVersionDataV1>(Bytes, *VersionOffset);
        if (!Version)
        {
            Result.Status = EPluginScanStatus::VersionExportOutOfBounds;
            return Result;
        }
        Result.Version = *Version;
        if (Version->DataVersion != PluginVersionDataVersion)
        {
            Result.Status = EPluginScanStatus::UnsupportedVersionDeclaration;
            return Result;
        }
        const auto Name = BoundedString(Version->Name);
        const auto Author = BoundedString(Version->Author);
        if (!Name || Name->empty())
        {
            Result.Status = EPluginScanStatus::MissingPluginName;
            return Result;
        }
        Result.Name = *Name;
        Result.Author = Author.value_or(std::string{});
        Result.Status = EPluginScanStatus::Ok;
        return Result;
    }

    auto PluginScanStatusName(const EPluginScanStatus Status) -> const char*
    {
        switch (Status)
        {
        case EPluginScanStatus::Ok: return "ok";
        case EPluginScanStatus::FileReadFailed: return "file_read_failed";
        case EPluginScanStatus::FileTooLarge: return "file_too_large";
        case EPluginScanStatus::NotPortableExecutable: return "not_portable_executable";
        case EPluginScanStatus::WrongArchitecture: return "wrong_architecture";
        case EPluginScanStatus::MalformedPortableExecutable: return "malformed_portable_executable";
        case EPluginScanStatus::MissingVersionExport: return "missing_version_export";
        case EPluginScanStatus::VersionExportOutOfBounds: return "version_export_out_of_bounds";
        case EPluginScanStatus::UnsupportedVersionDeclaration:
            return "unsupported_version_declaration";
        case EPluginScanStatus::MissingPluginName: return "missing_plugin_name";
        default: return "unknown_status";
        }
    }

    auto EvaluateCompatibility(const FPluginScanResult& Plugin,
                               const FPluginCompatibilityContext& Context)
            -> FPluginCompatibilityResult
    {
        if (Plugin.Status != EPluginScanStatus::Ok)
        {
            return {EPluginCompatibilityDisposition::StructuralBlocker,
                    CompatibilityReasonStructuralScanFailure};
        }

        std::uint32_t Reasons = CompatibilityReasonNone;
        if (!Plugin.HasLoad)
        {
            Reasons |= CompatibilityReasonNoLoadPhase;
        }
        if (Plugin.HasPreload && !Context.PreloadTimingAvailable)
        {
            Reasons |= CompatibilityReasonPreloadTimingUnavailable;
        }

        const auto AddressIndependent =
                (Plugin.Version.AddressIndependence &
                 (AddressIndependenceSignatures | AddressIndependenceAddressLibrary)) != 0;
        const auto StructureIndependent =
                (Plugin.Version.StructureIndependence &
                 (StructureIndependenceNoStructs | StructureIndependenceInitialLayout)) != 0;
        const auto VersionIndependent =
                AddressIndependent && StructureIndependent && Plugin.Version.ReservedBreaking == 0;

        if ((Plugin.Version.AddressIndependence & AddressIndependenceAddressLibrary) != 0 &&
            !Context.AddressLibraryAvailable)
        {
            Reasons |= CompatibilityReasonAddressLibraryUnavailable;
        }
        if (!VersionIndependent)
        {
            const auto Compatible = std::find(Plugin.Version.CompatibleVersions.begin(),
                                              Plugin.Version.CompatibleVersions.end(),
                                              Context.RuntimeVersion) !=
                                    Plugin.Version.CompatibleVersions.end();
            if (!Compatible)
            {
                Reasons |= CompatibilityReasonRuntimeMismatch;
            }
        }
        if (Plugin.Version.ScriptExtenderVersionRequired >
            Context.EmulatedScriptExtenderVersion)
        {
            Reasons |= CompatibilityReasonScriptExtenderTooOld;
        }

        if ((Reasons & CompatibilityReasonAddressLibraryUnavailable) != 0)
        {
            return {EPluginCompatibilityDisposition::StructuralBlocker, Reasons};
        }

        return {Reasons == CompatibilityReasonNone
                        ? EPluginCompatibilityDisposition::Verified
                        : EPluginCompatibilityDisposition::UnverifiedAttempt,
                Reasons};
    }

    auto PluginCompatibilityDispositionName(const EPluginCompatibilityDisposition Disposition)
            -> const char*
    {
        switch (Disposition)
        {
        case EPluginCompatibilityDisposition::Verified: return "verified";
        case EPluginCompatibilityDisposition::UnverifiedAttempt: return "unverified_attempt";
        case EPluginCompatibilityDisposition::StructuralBlocker: return "structural_blocker";
        default: return "unknown_disposition";
        }
    }
} // namespace RC::UNBSE::OBSE64
