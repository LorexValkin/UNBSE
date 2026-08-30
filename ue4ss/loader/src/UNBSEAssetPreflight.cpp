#include <UNBSEAssetPreflight.hpp>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cwctype>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <vector>

namespace RC::UNBSE::Preflight
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr std::array<std::uint8_t, 16> IoStoreMagic{
                '-', '=', '=', '-', '-', '=', '=', '-', '-', '=', '=', '-', '-', '=', '=', '-'};
        constexpr std::array<std::uint8_t, 4> PakMagic{0xE1, 0x12, 0x6F, 0x5A};
        constexpr std::array<std::uint8_t, 4> ContainerHeaderMagic{'n', 'C', 'o', 'I'};
        constexpr std::uint32_t SupportedTocVersion = 5;
        constexpr std::uint32_t SupportedTocHeaderBytes = 144;
        constexpr std::uint32_t SupportedCompressionBlockEntryBytes = 12;
        constexpr std::uint32_t SupportedCompressionMethodNameBytes = 32;
        constexpr std::uint32_t SupportedCompressionBlockBytes = 64 * 1024;
        constexpr std::uint32_t SupportedContainerHeaderVersion = 3;
        constexpr std::uint32_t SupportedPakVersion = 11;
        constexpr std::uint8_t IndexedContainerFlag = 1u << 3u;
        constexpr std::uint8_t ContainerHeaderChunkType = 6;
        constexpr std::uint64_t MaximumContainerHeaderBytes = 64ull * 1024ull * 1024ull;
        constexpr std::uint64_t MaximumPakIndexHashBytes = 64ull * 1024ull * 1024ull;

        struct FCaseInsensitiveLess
        {
            auto operator()(const std::wstring& Left, const std::wstring& Right) const -> bool
            {
                return _wcsicmp(Left.c_str(), Right.c_str()) < 0;
            }
        };

        struct FIoStoreHeader
        {
            std::uint32_t Version{};
            std::uint32_t HeaderBytes{};
            std::uint32_t EntryCount{};
            std::uint32_t CompressionBlockCount{};
            std::uint32_t CompressionBlockEntryBytes{};
            std::uint32_t CompressionMethodCount{};
            std::uint32_t CompressionMethodNameBytes{};
            std::uint32_t CompressionBlockBytes{};
            std::uint32_t DirectoryIndexBytes{};
            std::uint32_t PartitionCount{};
            std::uint64_t ContainerId{};
            std::uint8_t ContainerFlags{};
            std::uint32_t PerfectHashSeedCount{};
            std::uint64_t PartitionBytes{};
            std::uint32_t PerfectHashOverflowCount{};
        };

        struct FPakFooter
        {
            std::uint32_t Version{};
            std::uint64_t IndexOffset{};
            std::uint64_t IndexBytes{};
            std::array<std::uint8_t, 20> IndexHash{};
            std::uint64_t FooterOffset{};
        };

        enum class EContainerHeaderRead
        {
            Verified,
            Compressed,
            Missing,
            Invalid
        };

        auto FoldPath(const fs::path& Path) -> std::wstring
        {
            auto Value = Path.lexically_normal().wstring();
            std::transform(Value.begin(), Value.end(), Value.begin(),
                           [](const wchar_t Character) {
                               return static_cast<wchar_t>(towlower(Character));
                           });
            return Value;
        }

        template <typename T>
        auto ReadLittle(const std::span<const std::uint8_t> Bytes,
                        const std::size_t Offset) -> std::optional<T>
        {
            static_assert(std::is_unsigned_v<T>);
            if (Offset > Bytes.size() || sizeof(T) > Bytes.size() - Offset)
            {
                return std::nullopt;
            }
            T Result{};
            for (std::size_t Index = 0; Index < sizeof(T); ++Index)
            {
                Result |= static_cast<T>(Bytes[Offset + Index]) << (Index * 8u);
            }
            return Result;
        }

        auto ReadVariableLittle(const std::span<const std::uint8_t> Bytes) -> std::uint64_t
        {
            std::uint64_t Result{};
            for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
            {
                Result |= static_cast<std::uint64_t>(Bytes[Index]) << (Index * 8u);
            }
            return Result;
        }

        auto ReadVariableBig(const std::span<const std::uint8_t> Bytes) -> std::uint64_t
        {
            std::uint64_t Result{};
            for (const auto Byte : Bytes)
            {
                Result = (Result << 8u) | Byte;
            }
            return Result;
        }

        auto CheckedAdd(const std::uint64_t Left,
                        const std::uint64_t Right) -> std::optional<std::uint64_t>
        {
            if (Right > std::numeric_limits<std::uint64_t>::max() - Left)
            {
                return std::nullopt;
            }
            return Left + Right;
        }

        auto CheckedMultiply(const std::uint64_t Left,
                             const std::uint64_t Right) -> std::optional<std::uint64_t>
        {
            if (Left != 0 && Right > std::numeric_limits<std::uint64_t>::max() / Left)
            {
                return std::nullopt;
            }
            return Left * Right;
        }

        auto ReadRange(std::ifstream& Input,
                       const std::uint64_t Offset,
                       const std::size_t Bytes) -> std::optional<std::vector<std::uint8_t>>
        {
            if (Offset > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::streamoff>::max()) ||
                Bytes > static_cast<std::size_t>(
                                std::numeric_limits<std::streamsize>::max()))
            {
                return std::nullopt;
            }
            Input.clear();
            Input.seekg(static_cast<std::streamoff>(Offset), std::ios::beg);
            if (!Input)
            {
                return std::nullopt;
            }
            std::vector<std::uint8_t> Result(Bytes);
            Input.read(reinterpret_cast<char*>(Result.data()),
                       static_cast<std::streamsize>(Result.size()));
            if (Input.gcount() != static_cast<std::streamsize>(Result.size()))
            {
                return std::nullopt;
            }
            return Result;
        }

        auto ReadIoStoreHeader(const fs::path& Path,
                               std::wstring& Error) -> std::optional<FIoStoreHeader>
        {
            std::ifstream Input{Path, std::ios::binary};
            const auto Prefix = Input ? ReadRange(Input, 0, SupportedTocHeaderBytes)
                                      : std::nullopt;
            if (!Prefix)
            {
                Error = L"cannot read the complete IoStore header";
                return std::nullopt;
            }
            if (!std::equal(IoStoreMagic.begin(), IoStoreMagic.end(), Prefix->begin()))
            {
                Error = L"has an invalid IoStore magic value";
                return std::nullopt;
            }
            FIoStoreHeader Result{};
            const auto Bytes = std::span{*Prefix};
            const auto Version = ReadLittle<std::uint32_t>(Bytes, 0x10);
            const auto HeaderBytes = ReadLittle<std::uint32_t>(Bytes, 0x14);
            const auto EntryCount = ReadLittle<std::uint32_t>(Bytes, 0x18);
            const auto BlockCount = ReadLittle<std::uint32_t>(Bytes, 0x1C);
            const auto BlockEntryBytes = ReadLittle<std::uint32_t>(Bytes, 0x20);
            const auto MethodCount = ReadLittle<std::uint32_t>(Bytes, 0x24);
            const auto MethodNameBytes = ReadLittle<std::uint32_t>(Bytes, 0x28);
            const auto BlockBytes = ReadLittle<std::uint32_t>(Bytes, 0x2C);
            const auto DirectoryBytes = ReadLittle<std::uint32_t>(Bytes, 0x30);
            const auto PartitionCount = ReadLittle<std::uint32_t>(Bytes, 0x34);
            const auto ContainerId = ReadLittle<std::uint64_t>(Bytes, 0x38);
            const auto PerfectHashSeeds = ReadLittle<std::uint32_t>(Bytes, 0x54);
            const auto PartitionBytes = ReadLittle<std::uint64_t>(Bytes, 0x58);
            const auto PerfectHashOverflow = ReadLittle<std::uint32_t>(Bytes, 0x60);
            if (!Version || !HeaderBytes || !EntryCount || !BlockCount ||
                !BlockEntryBytes || !MethodCount || !MethodNameBytes || !BlockBytes ||
                !DirectoryBytes || !PartitionCount || !ContainerId || !PerfectHashSeeds ||
                !PartitionBytes || !PerfectHashOverflow)
            {
                Error = L"has a truncated IoStore header";
                return std::nullopt;
            }
            Result.Version = *Version;
            Result.HeaderBytes = *HeaderBytes;
            Result.EntryCount = *EntryCount;
            Result.CompressionBlockCount = *BlockCount;
            Result.CompressionBlockEntryBytes = *BlockEntryBytes;
            Result.CompressionMethodCount = *MethodCount;
            Result.CompressionMethodNameBytes = *MethodNameBytes;
            Result.CompressionBlockBytes = *BlockBytes;
            Result.DirectoryIndexBytes = *DirectoryBytes;
            Result.PartitionCount = *PartitionCount;
            Result.ContainerId = *ContainerId;
            Result.ContainerFlags = (*Prefix)[0x50];
            Result.PerfectHashSeedCount = *PerfectHashSeeds;
            Result.PartitionBytes = *PartitionBytes;
            Result.PerfectHashOverflowCount = *PerfectHashOverflow;

            std::error_code FileError{};
            const auto FileBytes = fs::file_size(Path, FileError);
            const auto ChunkIdBytes = CheckedMultiply(Result.EntryCount, 12);
            const auto OffsetLengthBytes = CheckedMultiply(Result.EntryCount, 10);
            const auto BlockTableBytes = CheckedMultiply(
                    Result.CompressionBlockCount, Result.CompressionBlockEntryBytes);
            const auto PerfectHashSeedBytes = CheckedMultiply(
                    Result.PerfectHashSeedCount, sizeof(std::uint32_t));
            const auto PerfectHashOverflowBytes = CheckedMultiply(
                    Result.PerfectHashOverflowCount, sizeof(std::uint32_t));
            auto MinimumBytes = ChunkIdBytes && OffsetLengthBytes && BlockTableBytes
                                                && PerfectHashSeedBytes &&
                                                PerfectHashOverflowBytes
                                        ? CheckedAdd(Result.HeaderBytes, *ChunkIdBytes)
                                        : std::nullopt;
            if (MinimumBytes)
            {
                MinimumBytes = CheckedAdd(*MinimumBytes, *OffsetLengthBytes);
            }
            if (MinimumBytes)
            {
                MinimumBytes = CheckedAdd(*MinimumBytes, *PerfectHashSeedBytes);
            }
            if (MinimumBytes)
            {
                MinimumBytes = CheckedAdd(*MinimumBytes, *PerfectHashOverflowBytes);
            }
            if (MinimumBytes)
            {
                MinimumBytes = CheckedAdd(*MinimumBytes, *BlockTableBytes);
            }
            if (FileError || Result.HeaderBytes < SupportedTocHeaderBytes ||
                Result.PartitionCount == 0 || Result.PartitionCount > 64 ||
                Result.CompressionBlockBytes == 0 || !MinimumBytes ||
                *MinimumBytes > FileBytes)
            {
                Error = L"has impossible IoStore counts or table bounds";
                return std::nullopt;
            }
            return Result;
        }

        auto UcasPartitionPath(const fs::path& TocPath, const std::uint32_t Partition)
                -> fs::path
        {
            auto Result = TocPath;
            Result.replace_extension();
            if (Partition != 0)
            {
                Result += L"_s" + std::to_wstring(Partition);
            }
            Result += L".ucas";
            return Result;
        }

        auto ReadContainerHeader(const fs::path& TocPath,
                                 const FIoStoreHeader& Header,
                                 std::uint32_t& Version,
                                 std::uint32_t& PackageCount,
                                 std::wstring& Error) -> EContainerHeaderRead
        {
            if ((Header.ContainerFlags & IndexedContainerFlag) == 0)
            {
                return EContainerHeaderRead::Missing;
            }
            std::ifstream Toc{TocPath, std::ios::binary};
            if (!Toc)
            {
                Error = L"cannot reopen the IoStore TOC";
                return EContainerHeaderRead::Invalid;
            }
            std::optional<std::uint32_t> HeaderEntry{};
            constexpr std::uint32_t ChunkIdsPerRead = 4096;
            for (std::uint32_t First = 0; First < Header.EntryCount;
                 First += ChunkIdsPerRead)
            {
                const auto Count = std::min(
                        ChunkIdsPerRead, Header.EntryCount - First);
                const auto Offset = static_cast<std::uint64_t>(Header.HeaderBytes) +
                                    static_cast<std::uint64_t>(First) * 12;
                const auto ChunkIds = ReadRange(
                        Toc, Offset, static_cast<std::size_t>(Count) * 12);
                if (!ChunkIds)
                {
                    Error = L"cannot read an IoStore chunk identifier";
                    return EContainerHeaderRead::Invalid;
                }
                for (std::uint32_t Relative = 0; Relative < Count; ++Relative)
                {
                    const auto ChunkOffset = static_cast<std::size_t>(Relative) * 12;
                    const auto ChunkContainer = ReadLittle<std::uint64_t>(
                            *ChunkIds, ChunkOffset);
                    if (ChunkContainer && *ChunkContainer == Header.ContainerId &&
                        (*ChunkIds)[ChunkOffset + 11] == ContainerHeaderChunkType)
                    {
                        HeaderEntry = First + Relative;
                        break;
                    }
                }
                if (HeaderEntry)
                {
                    break;
                }
            }
            if (!HeaderEntry)
            {
                Error = L"indexed IoStore container has no container-header chunk";
                return EContainerHeaderRead::Invalid;
            }
            const auto OffsetTable = static_cast<std::uint64_t>(Header.HeaderBytes) +
                                     static_cast<std::uint64_t>(Header.EntryCount) * 12;
            const auto Entry = ReadRange(
                    Toc, OffsetTable + static_cast<std::uint64_t>(*HeaderEntry) * 10, 10);
            if (!Entry)
            {
                Error = L"cannot read the container-header offset and length";
                return EContainerHeaderRead::Invalid;
            }
            const auto VirtualOffset = ReadVariableBig(std::span{*Entry}.first(5));
            const auto HeaderLength = ReadVariableBig(std::span{*Entry}.subspan(5, 5));
            if (HeaderLength < 20 || HeaderLength > MaximumContainerHeaderBytes ||
                Header.CompressionBlockBytes == 0)
            {
                Error = L"container-header chunk has an invalid length";
                return EContainerHeaderRead::Invalid;
            }
            const auto ChunkEnd = CheckedAdd(VirtualOffset, HeaderLength);
            if (!ChunkEnd || *ChunkEnd == 0)
            {
                Error = L"container-header chunk range overflows";
                return EContainerHeaderRead::Invalid;
            }
            const auto FirstBlock = VirtualOffset / Header.CompressionBlockBytes;
            const auto LastBlock = (*ChunkEnd - 1) / Header.CompressionBlockBytes;
            if (LastBlock >= Header.CompressionBlockCount)
            {
                Error = L"container-header chunk references a missing compression block";
                return EContainerHeaderRead::Invalid;
            }
            const auto BlockTable = OffsetTable +
                                    static_cast<std::uint64_t>(Header.EntryCount) * 10 +
                                    static_cast<std::uint64_t>(Header.PerfectHashSeedCount) * 4 +
                                    static_cast<std::uint64_t>(
                                            Header.PerfectHashOverflowCount) * 4;
            std::vector<std::uint8_t> Chunk{};
            Chunk.reserve(static_cast<std::size_t>(HeaderLength));
            for (auto BlockIndex = FirstBlock; BlockIndex <= LastBlock; ++BlockIndex)
            {
                const auto Block = ReadRange(
                        Toc, BlockTable + BlockIndex * Header.CompressionBlockEntryBytes,
                        Header.CompressionBlockEntryBytes);
                if (!Block || Block->size() < SupportedCompressionBlockEntryBytes)
                {
                    Error = L"cannot read a container-header compression block";
                    return EContainerHeaderRead::Invalid;
                }
                const auto PhysicalOffset = ReadVariableLittle(std::span{*Block}.first(5));
                const auto CompressedBytes = ReadVariableLittle(std::span{*Block}.subspan(5, 3));
                const auto UncompressedBytes = ReadVariableLittle(std::span{*Block}.subspan(8, 3));
                const auto Method = (*Block)[11];
                if (Method != 0)
                {
                    return EContainerHeaderRead::Compressed;
                }
                if (CompressedBytes == 0 || CompressedBytes != UncompressedBytes ||
                    UncompressedBytes > Header.CompressionBlockBytes)
                {
                    Error = L"uncompressed container-header block has invalid sizes";
                    return EContainerHeaderRead::Invalid;
                }
                const auto Partition = Header.PartitionBytes ==
                                                       std::numeric_limits<std::uint64_t>::max()
                                               ? 0
                                               : PhysicalOffset / Header.PartitionBytes;
                const auto PartitionOffset = Header.PartitionBytes ==
                                                     std::numeric_limits<std::uint64_t>::max()
                                             ? PhysicalOffset
                                             : PhysicalOffset % Header.PartitionBytes;
                if (Partition >= Header.PartitionCount)
                {
                    Error = L"container-header block references a missing UCAS partition";
                    return EContainerHeaderRead::Invalid;
                }
                std::ifstream Ucas{UcasPartitionPath(TocPath, static_cast<std::uint32_t>(Partition)),
                                   std::ios::binary};
                const auto Physical = Ucas ? ReadRange(
                                                      Ucas, PartitionOffset,
                                                      static_cast<std::size_t>(CompressedBytes))
                                           : std::nullopt;
                if (!Physical)
                {
                    Error = L"cannot read the container-header bytes from UCAS";
                    return EContainerHeaderRead::Invalid;
                }
                const auto BlockVirtual = BlockIndex * Header.CompressionBlockBytes;
                const auto CopyBegin = std::max(VirtualOffset, BlockVirtual) - BlockVirtual;
                const auto CopyEnd = std::min(*ChunkEnd, BlockVirtual + UncompressedBytes) -
                                     BlockVirtual;
                if (CopyBegin > CopyEnd || CopyEnd > Physical->size())
                {
                    Error = L"container-header bytes exceed their compression block";
                    return EContainerHeaderRead::Invalid;
                }
                Chunk.insert(Chunk.end(), Physical->begin() + static_cast<std::ptrdiff_t>(CopyBegin),
                             Physical->begin() + static_cast<std::ptrdiff_t>(CopyEnd));
            }
            if (Chunk.size() != HeaderLength ||
                !std::equal(ContainerHeaderMagic.begin(), ContainerHeaderMagic.end(),
                            Chunk.begin()))
            {
                Error = L"container-header chunk has invalid serialized bytes";
                return EContainerHeaderRead::Invalid;
            }
            const auto ParsedVersion = ReadLittle<std::uint32_t>(Chunk, 4);
            const auto ParsedContainer = ReadLittle<std::uint64_t>(Chunk, 8);
            const auto ParsedPackages = ReadLittle<std::uint32_t>(Chunk, 16);
            if (!ParsedVersion || !ParsedContainer || !ParsedPackages ||
                *ParsedContainer != Header.ContainerId)
            {
                Error = L"container-header identity is inconsistent with its TOC";
                return EContainerHeaderRead::Invalid;
            }
            Version = *ParsedVersion;
            PackageCount = *ParsedPackages;
            return EContainerHeaderRead::Verified;
        }

        auto ReadPakFooter(const fs::path& Path,
                           std::wstring& Error) -> std::optional<FPakFooter>
        {
            std::error_code FileError{};
            const auto FileBytes = fs::file_size(Path, FileError);
            if (FileError || FileBytes < 44)
            {
                Error = L"is too small to contain a valid Unreal pak footer";
                return std::nullopt;
            }
            const auto TailBytes = static_cast<std::size_t>(std::min<std::uint64_t>(512, FileBytes));
            std::ifstream Input{Path, std::ios::binary};
            const auto Tail = Input ? ReadRange(Input, FileBytes - TailBytes, TailBytes)
                                    : std::nullopt;
            if (!Tail)
            {
                Error = L"cannot read the Unreal pak footer";
                return std::nullopt;
            }
            for (std::size_t Index = Tail->size() - 4;; --Index)
            {
                if (std::equal(PakMagic.begin(), PakMagic.end(), Tail->begin() + Index) &&
                    Index + 44 <= Tail->size())
                {
                    const auto Version = ReadLittle<std::uint32_t>(*Tail, Index + 4);
                    const auto IndexOffset = ReadLittle<std::uint64_t>(*Tail, Index + 8);
                    const auto IndexBytes = ReadLittle<std::uint64_t>(*Tail, Index + 16);
                    const auto FooterOffset = FileBytes - TailBytes + Index;
                    const auto IndexEnd = IndexOffset && IndexBytes
                                                  ? CheckedAdd(*IndexOffset, *IndexBytes)
                                                  : std::nullopt;
                    if (Version && *Version >= 1 && *Version <= 12 && IndexEnd &&
                        *IndexBytes != 0 && *IndexEnd <= FooterOffset)
                    {
                        FPakFooter Result{};
                        Result.Version = *Version;
                        Result.IndexOffset = *IndexOffset;
                        Result.IndexBytes = *IndexBytes;
                        Result.FooterOffset = FooterOffset;
                        std::copy_n(Tail->begin() + Index + 24, Result.IndexHash.size(),
                                    Result.IndexHash.begin());
                        return Result;
                    }
                }
                if (Index == 0)
                {
                    break;
                }
            }
            Error = L"has no valid Unreal pak footer or bounded index";
            return std::nullopt;
        }

        auto HashPakIndex(const fs::path& Path,
                          const FPakFooter& Footer) -> std::optional<std::array<std::uint8_t, 20>>
        {
            if (Footer.IndexBytes > MaximumPakIndexHashBytes)
            {
                return std::nullopt;
            }
            BCRYPT_ALG_HANDLE Algorithm{};
            BCRYPT_HASH_HANDLE Hash{};
            std::vector<std::uint8_t> Object{};
            std::array<std::uint8_t, 20> Digest{};
            DWORD ObjectBytes{};
            DWORD ResultBytes{};
            auto Cleanup = [&]() {
                if (Hash)
                {
                    BCryptDestroyHash(Hash);
                }
                if (Algorithm)
                {
                    BCryptCloseAlgorithmProvider(Algorithm, 0);
                }
            };
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                        &Algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0)) ||
                !BCRYPT_SUCCESS(BCryptGetProperty(
                        Algorithm, BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&ObjectBytes), sizeof(ObjectBytes),
                        &ResultBytes, 0)))
            {
                Cleanup();
                return std::nullopt;
            }
            Object.resize(ObjectBytes);
            if (!BCRYPT_SUCCESS(BCryptCreateHash(
                        Algorithm, &Hash, Object.data(), static_cast<ULONG>(Object.size()),
                        nullptr, 0, 0)))
            {
                Cleanup();
                return std::nullopt;
            }
            std::ifstream Input{Path, std::ios::binary};
            if (!Input || Footer.IndexOffset > static_cast<std::uint64_t>(
                                                     std::numeric_limits<std::streamoff>::max()))
            {
                Cleanup();
                return std::nullopt;
            }
            Input.seekg(static_cast<std::streamoff>(Footer.IndexOffset), std::ios::beg);
            std::vector<std::uint8_t> Buffer(1024 * 1024);
            auto Remaining = Footer.IndexBytes;
            while (Remaining != 0)
            {
                const auto Request = static_cast<std::size_t>(
                        std::min<std::uint64_t>(Remaining, Buffer.size()));
                Input.read(reinterpret_cast<char*>(Buffer.data()),
                           static_cast<std::streamsize>(Request));
                if (Input.gcount() != static_cast<std::streamsize>(Request) ||
                    !BCRYPT_SUCCESS(BCryptHashData(
                            Hash, Buffer.data(), static_cast<ULONG>(Request), 0)))
                {
                    Cleanup();
                    return std::nullopt;
                }
                Remaining -= Request;
            }
            const auto Finished = BCryptFinishHash(
                    Hash, Digest.data(), static_cast<ULONG>(Digest.size()), 0);
            Cleanup();
            return BCRYPT_SUCCESS(Finished) ? std::optional{Digest} : std::nullopt;
        }

        auto Hex(const std::span<const std::uint8_t> Bytes) -> std::wstring
        {
            constexpr std::wstring_view Digits = L"0123456789ABCDEF";
            std::wstring Result{};
            Result.reserve(Bytes.size() * 2);
            for (const auto Byte : Bytes)
            {
                Result.push_back(Digits[(Byte >> 4u) & 0xFu]);
                Result.push_back(Digits[Byte & 0xFu]);
            }
            return Result;
        }

        auto FileIdentity(const fs::path& Path, const std::wstring_view Evidence) -> std::wstring
        {
            std::error_code SizeError{};
            const auto Bytes = fs::file_size(Path, SizeError);
            std::error_code TimeError{};
            const auto WriteTime = fs::last_write_time(Path, TimeError);
            std::wostringstream Result{};
            Result << FoldPath(Path) << L'|' << (SizeError ? 0 : Bytes) << L'|'
                   << (TimeError ? 0 : WriteTime.time_since_epoch().count()) << L'|'
                   << Evidence;
            return Result.str();
        }

        auto AddWarning(FAssetContainerReview& Review,
                        const fs::path& Path,
                        std::wstring Reason,
                        std::wstring Evidence,
                        const bool FingerprintPrimaryFile = false) -> void
        {
            Review.Warnings.push_back(
                    {Path.stem().wstring(), Path, std::move(Reason), Evidence,
                     FileIdentity(Path, Evidence), FingerprintPrimaryFile});
        }
    } // namespace

    auto ReviewAssetContainers(const fs::path& GameBinaryDirectory)
            -> FAssetContainerReview
    {
        FAssetContainerReview Review{};
        const auto ProjectRoot = GameBinaryDirectory.parent_path().parent_path();
        const auto PaksDirectory = ProjectRoot / L"Content" / L"Paks";
        std::error_code Error{};
        if (!fs::is_directory(PaksDirectory, Error))
        {
            return Review;
        }

        std::vector<fs::path> TocPaths{};
        std::vector<fs::path> UcasPaths{};
        std::vector<fs::path> PakPaths{};
        fs::recursive_directory_iterator Iterator{
                PaksDirectory, fs::directory_options::skip_permission_denied, Error};
        const fs::recursive_directory_iterator End{};
        for (; !Error && Iterator != End; Iterator.increment(Error))
        {
            if (!Iterator->is_regular_file(Error))
            {
                continue;
            }
            auto Extension = Iterator->path().extension().wstring();
            std::transform(Extension.begin(), Extension.end(), Extension.begin(),
                           [](const wchar_t Character) {
                               return static_cast<wchar_t>(towlower(Character));
                           });
            if (Extension == L".utoc")
            {
                TocPaths.push_back(Iterator->path());
            }
            else if (Extension == L".ucas")
            {
                UcasPaths.push_back(Iterator->path());
            }
            else if (Extension == L".pak")
            {
                PakPaths.push_back(Iterator->path());
            }
        }
        if (Error)
        {
            AddWarning(Review, PaksDirectory,
                       L"asset container directory could not be enumerated completely",
                       L"filesystem error " + std::to_wstring(Error.value()));
        }
        std::sort(TocPaths.begin(), TocPaths.end());
        std::sort(UcasPaths.begin(), UcasPaths.end());
        std::sort(PakPaths.begin(), PakPaths.end());
        Review.TocContainers = TocPaths.size();
        Review.PakContainers = PakPaths.size();

        std::set<std::wstring, FCaseInsensitiveLess> ClaimedUcas{};
        std::map<std::uint64_t, fs::path> ContainerIds{};
        for (const auto& TocPath : TocPaths)
        {
            std::wstring ParseError{};
            const auto Header = ReadIoStoreHeader(TocPath, ParseError);
            if (!Header)
            {
                AddWarning(Review, TocPath, std::move(ParseError), L"IoStore header invalid",
                           true);
                continue;
            }
            std::wostringstream Envelope{};
            Envelope << L"TOC version " << Header->Version << L", header "
                     << Header->HeaderBytes << L" bytes, block entry "
                     << Header->CompressionBlockEntryBytes << L" bytes, block size "
                     << Header->CompressionBlockBytes;
            if (Header->Version != SupportedTocVersion ||
                Header->HeaderBytes != SupportedTocHeaderBytes ||
                Header->CompressionBlockEntryBytes !=
                        SupportedCompressionBlockEntryBytes ||
                Header->CompressionMethodNameBytes !=
                        SupportedCompressionMethodNameBytes ||
                Header->CompressionBlockBytes != SupportedCompressionBlockBytes)
            {
                AddWarning(Review, TocPath,
                           L"IoStore format differs from the current retail container format",
                           Envelope.str(), true);
            }

            for (std::uint32_t Partition = 0; Partition < Header->PartitionCount; ++Partition)
            {
                const auto UcasPath = UcasPartitionPath(TocPath, Partition);
                ClaimedUcas.insert(FoldPath(UcasPath));
                std::error_code UcasError{};
                const auto UcasBytes = fs::file_size(UcasPath, UcasError);
                if (UcasError || UcasBytes == 0)
                {
                    AddWarning(Review, TocPath,
                               L"IoStore TOC is missing a non-empty UCAS partition",
                               UcasPath.filename().wstring(), true);
                }
                else if (Header->PartitionBytes !=
                                 std::numeric_limits<std::uint64_t>::max() &&
                         Partition + 1 < Header->PartitionCount &&
                         UcasBytes != Header->PartitionBytes)
                {
                    AddWarning(Review, TocPath,
                               L"IoStore UCAS partition size disagrees with its TOC",
                               UcasPath.filename().wstring() + L" is " +
                                       std::to_wstring(UcasBytes) + L" bytes; expected " +
                                       std::to_wstring(Header->PartitionBytes),
                               true);
                }
            }

            if (Header->ContainerId != std::numeric_limits<std::uint64_t>::max())
            {
                const auto Existing = ContainerIds.find(Header->ContainerId);
                if (Existing != ContainerIds.end())
                {
                    std::wostringstream Evidence{};
                    Evidence << L"container id 0x" << std::hex << std::uppercase
                             << Header->ContainerId << L" is also used by "
                             << Existing->second.filename().wstring();
                    AddWarning(Review, TocPath,
                               L"duplicate IoStore container identity can override another mod",
                               Evidence.str(), true);
                }
                else
                {
                    ContainerIds.emplace(Header->ContainerId, TocPath);
                }
            }

            std::uint32_t ContainerVersion{};
            std::uint32_t PackageCount{};
            std::wstring ContainerError{};
            const auto HeaderRead = ReadContainerHeader(
                    TocPath, *Header, ContainerVersion, PackageCount, ContainerError);
            if (HeaderRead == EContainerHeaderRead::Verified)
            {
                ++Review.SerializationVersionsVerified;
                if (ContainerVersion != SupportedContainerHeaderVersion)
                {
                    AddWarning(
                            Review, TocPath,
                            L"IoStore package serialization header differs from the current retail format",
                            L"container-header version " + std::to_wstring(ContainerVersion) +
                                    L"; expected NoExportInfo version 3; packages " +
                                    std::to_wstring(PackageCount),
                            true);
                }
            }
            else if (HeaderRead == EContainerHeaderRead::Compressed)
            {
                ++Review.SerializationVersionsUnverified;
            }
            else if (HeaderRead == EContainerHeaderRead::Invalid)
            {
                AddWarning(Review, TocPath, std::move(ContainerError), Envelope.str(), true);
            }
        }

        for (const auto& UcasPath : UcasPaths)
        {
            if (!ClaimedUcas.contains(FoldPath(UcasPath)))
            {
                AddWarning(Review, UcasPath,
                           L"UCAS payload has no matching IoStore TOC",
                           L"orphan payload cannot be mounted safely");
            }
        }

        for (const auto& PakPath : PakPaths)
        {
            std::wstring FooterError{};
            const auto Footer = ReadPakFooter(PakPath, FooterError);
            if (!Footer)
            {
                AddWarning(Review, PakPath, std::move(FooterError), L"pak footer invalid");
                continue;
            }
            const auto ActualHash = HashPakIndex(PakPath, *Footer);
            if (ActualHash)
            {
                ++Review.PakIndexesVerified;
            }
            else
            {
                ++Review.PakIndexesUnverified;
            }
            const auto HashMatches = ActualHash && *ActualHash == Footer->IndexHash;
            std::wostringstream Evidence{};
            Evidence << L"pak version " << Footer->Version << L", index "
                     << Footer->IndexBytes << L" bytes, SHA-1 " << Hex(Footer->IndexHash);
            if (Footer->Version != SupportedPakVersion)
            {
                AddWarning(Review, PakPath,
                           L"pak format version differs from the current retail format",
                           Evidence.str());
            }
            if (ActualHash && !HashMatches)
            {
                AddWarning(Review, PakPath,
                           L"pak index SHA-1 does not match its footer",
                           Evidence.str());
            }
        }
        return Review;
    }
} // namespace RC::UNBSE::Preflight
