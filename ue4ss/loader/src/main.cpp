#include <OBSE64PluginScanner.hpp>
#include <UNBSEAssetPreflight.hpp>

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr std::wstring_view GameExecutableName =
            L"OblivionRemastered-Win64-Shipping.exe";
    constexpr std::wstring_view SupportedRuntime = L"Steam 1.512.105.0";
    constexpr auto SupportedRuntimeVersion =
            RC::UNBSE::OBSE64::PackRuntimeVersion(1, 512, 105, 0);
    constexpr std::wstring_view SteamGameRelativePath =
            L"steamapps/common/Oblivion Remastered/OblivionRemastered/Binaries/Win64/"
            L"OblivionRemastered-Win64-Shipping.exe";
    constexpr std::wstring_view ExpectedGameSha256 =
            L"B7BE7E6EBE9424F6FDF274F5E7A59372DE103E043AB5B66C60E021C0C89DF457";
#define UNBSE_WIDEN_LITERAL_IMPL(Value) L##Value
#define UNBSE_WIDEN_LITERAL(Value) UNBSE_WIDEN_LITERAL_IMPL(Value)
    constexpr std::wstring_view ExpectedUE4SSSha256 =
            UNBSE_WIDEN_LITERAL(UNBSE_EXPECTED_UE4SS_SHA256);
    constexpr std::uintmax_t ExpectedUE4SSBytes = UNBSE_EXPECTED_UE4SS_BYTES;
    constexpr std::array<std::wstring_view, 3> ConflictingUE4SSLoaderNames{
            L"dwmapi.dll", L"ue4ss_loader.dll", L"ue4ss_dwmapi.dll"};
    constexpr std::string_view SettingsProfileMarker =
            "; UNBSE-Settings-Profile: 0.14.5";

    enum class EIniSettingPolicy
    {
        Required,
        DefaultNumericBoolean,
        DefaultTextBoolean,
    };

    struct FRequiredIniSetting
    {
        std::string_view Section{};
        std::string_view Key{};
        std::string_view Value{};
        EIniSettingPolicy Policy{EIniSettingPolicy::Required};
    };

    constexpr std::array RequiredIniSettings{
            FRequiredIniSetting{"General", "EnableHotReloadSystem", "0"},
            FRequiredIniSetting{"General", "UseCache", "1"},
            FRequiredIniSetting{"General", "InvalidateCacheIfDLLDiffers", "1"},
            FRequiredIniSetting{"General", "bUseUObjectArrayCache", "false",
                                EIniSettingPolicy::DefaultTextBoolean},
            FRequiredIniSetting{
                    "General", "DefaultExecuteInGameThreadMethod", "EngineTick"},
            FRequiredIniSetting{"Debug", "ConsoleEnabled", "1"},
            FRequiredIniSetting{
                    "ObjectDumper", "LoadAllAssetsBeforeDumpingObjects", "0"},
            FRequiredIniSetting{
                    "CXXHeaderGenerator", "LoadAllAssetsBeforeGeneratingCXXHeaders", "0"},
            FRequiredIniSetting{"Hooks", "HookEngineTick", "1"},
            FRequiredIniSetting{"Hooks", "EngineTickResolveMethod", "Scan"},
            FRequiredIniSetting{"Hooks", "HookGameViewportClientTick", "1"},
            FRequiredIniSetting{"Hooks", "HookUObjectProcessEvent", "1"},
            FRequiredIniSetting{"Hooks", "HookProcessInternal", "1"},
            FRequiredIniSetting{"Hooks", "HookProcessLocalScriptFunction", "1"},
            FRequiredIniSetting{"Hooks", "HookLoadMap", "1"},
            FRequiredIniSetting{
                    "UNBSE", "EnablePluginVersionWarning", "0",
                    EIniSettingPolicy::DefaultNumericBoolean},
            FRequiredIniSetting{
                    "UNBSE", "EnableAssetContainerWarning", "0",
                    EIniSettingPolicy::DefaultNumericBoolean}};

    [[nodiscard]] auto IsDefaultIniSetting(const FRequiredIniSetting& Setting)
            -> bool
    {
        return Setting.Policy != EIniSettingPolicy::Required;
    }

    [[nodiscard]] auto IsIniSettingValueValid(const FRequiredIniSetting& Setting,
                                              const std::string_view Value) -> bool
    {
        switch (Setting.Policy)
        {
        case EIniSettingPolicy::Required:
            return Value == Setting.Value;
        case EIniSettingPolicy::DefaultNumericBoolean:
            return Value == "0" || Value == "1";
        case EIniSettingPolicy::DefaultTextBoolean:
            return Value == "false" || Value == "true";
        }
        return false;
    }

    [[nodiscard]] auto IniSettingExpectation(const FRequiredIniSetting& Setting)
            -> std::string
    {
        switch (Setting.Policy)
        {
        case EIniSettingPolicy::Required:
            return "required '" + std::string{Setting.Value} + "'";
        case EIniSettingPolicy::DefaultNumericBoolean:
            return "expected 0 or 1; default '" + std::string{Setting.Value} + "'";
        case EIniSettingPolicy::DefaultTextBoolean:
            return "expected true or false; default '" + std::string{Setting.Value} +
                   "'";
        }
        return "invalid setting contract";
    }

    class FHandle
    {
      public:
        FHandle() = default;
        explicit FHandle(HANDLE Handle) : m_handle(Handle) {}
        ~FHandle()
        {
            if (m_handle && m_handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(m_handle);
            }
        }

        FHandle(const FHandle&) = delete;
        auto operator=(const FHandle&) -> FHandle& = delete;
        FHandle(FHandle&& Other) noexcept : m_handle(Other.release()) {}
        auto operator=(FHandle&& Other) noexcept -> FHandle&
        {
            if (this != &Other)
            {
                FHandle Temporary{std::move(Other)};
                swap(Temporary);
            }
            return *this;
        }

        [[nodiscard]] auto get() const -> HANDLE { return m_handle; }
        [[nodiscard]] explicit operator bool() const
        {
            return m_handle && m_handle != INVALID_HANDLE_VALUE;
        }
        [[nodiscard]] auto release() -> HANDLE
        {
            const auto Result = m_handle;
            m_handle = nullptr;
            return Result;
        }
        auto swap(FHandle& Other) noexcept -> void { std::swap(m_handle, Other.m_handle); }

      private:
        HANDLE m_handle{};
    };

    auto FormatWindowsError(const DWORD Error) -> std::wstring
    {
        wchar_t* Buffer{};
        const auto Length = FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, Error, 0, reinterpret_cast<wchar_t*>(&Buffer), 0, nullptr);
        std::wstring Result = Length && Buffer ? std::wstring{Buffer, Length}
                                               : L"Windows error " + std::to_wstring(Error);
        if (Buffer)
        {
            LocalFree(Buffer);
        }
        while (!Result.empty() && std::iswspace(Result.back()))
        {
            Result.pop_back();
        }
        return Result;
    }

    auto ToLower(std::wstring Value) -> std::wstring
    {
        std::transform(Value.begin(), Value.end(), Value.begin(),
                       [](const wchar_t Character) {
                           return static_cast<wchar_t>(std::towlower(Character));
                       });
        return Value;
    }

    auto GetSelfPath() -> fs::path
    {
        std::vector<wchar_t> Buffer(32768);
        const auto Length = GetModuleFileNameW(
                nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
        if (Length == 0 || Length >= Buffer.size())
        {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        return fs::path{std::wstring{Buffer.data(), Length}};
    }

    auto BytesToHex(const std::span<const std::uint8_t> Bytes) -> std::wstring
    {
        constexpr std::array Hex{L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7',
                                 L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F'};
        std::wstring Result;
        Result.reserve(Bytes.size() * 2);
        for (const auto Byte : Bytes)
        {
            Result.push_back(Hex[(Byte >> 4U) & 0xFU]);
            Result.push_back(Hex[Byte & 0xFU]);
        }
        return Result;
    }

    auto Sha256File(const fs::path& Path) -> std::wstring
    {
        BCRYPT_ALG_HANDLE Algorithm{};
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                    &Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        {
            throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
        }

        struct FAlgorithmCloser
        {
            BCRYPT_ALG_HANDLE Value{};
            ~FAlgorithmCloser()
            {
                if (Value)
                {
                    BCryptCloseAlgorithmProvider(Value, 0);
                }
            }
        } AlgorithmCloser{Algorithm};

        DWORD ObjectBytes{};
        DWORD ResultBytes{};
        if (!BCRYPT_SUCCESS(BCryptGetProperty(
                    Algorithm, BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&ObjectBytes), sizeof(ObjectBytes),
                    &ResultBytes, 0)))
        {
            throw std::runtime_error("BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed");
        }
        DWORD HashBytes{};
        if (!BCRYPT_SUCCESS(BCryptGetProperty(
                    Algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&HashBytes),
                    sizeof(HashBytes), &ResultBytes, 0)))
        {
            throw std::runtime_error("BCryptGetProperty(BCRYPT_HASH_LENGTH) failed");
        }

        std::vector<std::uint8_t> Object(ObjectBytes);
        BCRYPT_HASH_HANDLE Hash{};
        if (!BCRYPT_SUCCESS(BCryptCreateHash(Algorithm, &Hash, Object.data(),
                                             static_cast<ULONG>(Object.size()), nullptr, 0,
                                             0)))
        {
            throw std::runtime_error("BCryptCreateHash failed");
        }
        struct FHashCloser
        {
            BCRYPT_HASH_HANDLE Value{};
            ~FHashCloser()
            {
                if (Value)
                {
                    BCryptDestroyHash(Value);
                }
            }
        } HashCloser{Hash};

        std::ifstream Input{Path, std::ios::binary};
        if (!Input)
        {
            throw std::runtime_error("Unable to open file for hashing");
        }
        std::vector<char> Buffer(1024 * 1024);
        while (Input)
        {
            Input.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
            const auto Read = Input.gcount();
            if (Read > 0 && !BCRYPT_SUCCESS(BCryptHashData(
                                    Hash, reinterpret_cast<PUCHAR>(Buffer.data()),
                                    static_cast<ULONG>(Read), 0)))
            {
                throw std::runtime_error("BCryptHashData failed");
            }
        }
        if (!Input.eof())
        {
            throw std::runtime_error("Unable to read file for hashing");
        }

        std::vector<std::uint8_t> Digest(HashBytes);
        if (!BCRYPT_SUCCESS(BCryptFinishHash(
                    Hash, Digest.data(), static_cast<ULONG>(Digest.size()), 0)))
        {
            throw std::runtime_error("BCryptFinishHash failed");
        }
        return BytesToHex(Digest);
    }

    auto Sha256Text(const std::wstring_view Value) -> std::wstring
    {
        BCRYPT_ALG_HANDLE Algorithm{};
        BCRYPT_HASH_HANDLE Hash{};
        DWORD ObjectBytes{};
        DWORD HashBytes{};
        DWORD ResultBytes{};
        std::vector<std::uint8_t> Object{};
        std::vector<std::uint8_t> Digest{};
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
                    &Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !BCRYPT_SUCCESS(BCryptGetProperty(
                    Algorithm, BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&ObjectBytes), sizeof(ObjectBytes),
                    &ResultBytes, 0)) ||
            !BCRYPT_SUCCESS(BCryptGetProperty(
                    Algorithm, BCRYPT_HASH_LENGTH,
                    reinterpret_cast<PUCHAR>(&HashBytes), sizeof(HashBytes),
                    &ResultBytes, 0)))
        {
            Cleanup();
            throw std::runtime_error("Unable to initialize SHA-256 text hashing");
        }
        Object.resize(ObjectBytes);
        Digest.resize(HashBytes);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(
                    Algorithm, &Hash, Object.data(), static_cast<ULONG>(Object.size()),
                    nullptr, 0, 0)))
        {
            Cleanup();
            throw std::runtime_error("Unable to create SHA-256 text hash");
        }
        const auto Bytes = std::as_bytes(std::span{Value});
        if (Bytes.size() > std::numeric_limits<ULONG>::max() ||
            !BCRYPT_SUCCESS(BCryptHashData(
                    Hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(Bytes.data())),
                    static_cast<ULONG>(Bytes.size()), 0)) ||
            !BCRYPT_SUCCESS(BCryptFinishHash(
                    Hash, Digest.data(), static_cast<ULONG>(Digest.size()), 0)))
        {
            Cleanup();
            throw std::runtime_error("Unable to compute SHA-256 text hash");
        }
        Cleanup();
        return BytesToHex(Digest);
    }

    auto ValidatePinnedFile(const fs::path& Path, const std::uintmax_t ExpectedBytes,
                            const std::wstring_view ExpectedHash,
                            const std::wstring_view Label) -> bool
    {
        std::error_code Error{};
        if (!fs::is_regular_file(Path, Error))
        {
            std::wcerr << L"ERROR: " << Label << L" is missing: " << Path << L"\n";
            return false;
        }
        const auto Bytes = fs::file_size(Path, Error);
        if (Error || Bytes != ExpectedBytes)
        {
            std::wcerr << L"ERROR: " << Label << L" has an unexpected size: " << Path
                       << L"\n";
            return false;
        }
        const auto Hash = Sha256File(Path);
        if (Hash != ExpectedHash)
        {
            std::wcerr << L"ERROR: " << Label << L" has an unexpected SHA-256: " << Path
                       << L"\n";
            return false;
        }
        return true;
    }

    auto ReadRegistryString(HKEY Root, const wchar_t* Subkey, const wchar_t* Name)
            -> std::optional<std::wstring>
    {
        DWORD Type{};
        DWORD Bytes{};
        if (RegGetValueW(Root, Subkey, Name, RRF_RT_REG_SZ, &Type, nullptr, &Bytes) !=
                    ERROR_SUCCESS ||
            Bytes < sizeof(wchar_t))
        {
            return std::nullopt;
        }
        std::vector<wchar_t> Buffer(Bytes / sizeof(wchar_t));
        if (RegGetValueW(Root, Subkey, Name, RRF_RT_REG_SZ, &Type, Buffer.data(),
                         &Bytes) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        return std::wstring{Buffer.data()};
    }

    auto ParseSteamLibraryPaths(const fs::path& SteamRoot) -> std::vector<fs::path>
    {
        std::vector<fs::path> Results{SteamRoot};
        std::wifstream Input{SteamRoot / L"steamapps" / L"libraryfolders.vdf"};
        std::wstring Line;
        while (std::getline(Input, Line))
        {
            const auto Key = Line.find(L"\"path\"");
            if (Key == std::wstring::npos)
            {
                continue;
            }
            const auto ValueBegin = Line.find(L'"', Key + 6);
            const auto ValueEnd = ValueBegin == std::wstring::npos
                                          ? std::wstring::npos
                                          : Line.find(L'"', ValueBegin + 1);
            if (ValueBegin == std::wstring::npos || ValueEnd == std::wstring::npos)
            {
                continue;
            }
            auto Value = Line.substr(ValueBegin + 1, ValueEnd - ValueBegin - 1);
            std::wstring Unescaped;
            Unescaped.reserve(Value.size());
            for (std::size_t Index = 0; Index < Value.size(); ++Index)
            {
                if (Value[Index] == L'\\' && Index + 1 < Value.size() &&
                    Value[Index + 1] == L'\\')
                {
                    ++Index;
                }
                Unescaped.push_back(Value[Index]);
            }
            Results.emplace_back(std::move(Unescaped));
        }
        return Results;
    }

    auto FindGameExecutable(const std::optional<fs::path>& Override,
                            const fs::path& SelfDirectory) -> std::optional<fs::path>
    {
        std::vector<fs::path> Candidates{};
        if (Override)
        {
            Candidates.push_back(*Override);
        }
        Candidates.push_back(fs::current_path() / GameExecutableName);
        Candidates.push_back(SelfDirectory / GameExecutableName);

        if (const auto SteamPath = ReadRegistryString(
                    HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", L"SteamPath"))
        {
            for (const auto& Library : ParseSteamLibraryPaths(*SteamPath))
            {
                Candidates.push_back(Library / SteamGameRelativePath);
            }
        }

        for (const auto& Candidate : Candidates)
        {
            std::error_code Error{};
            if (fs::is_regular_file(Candidate, Error))
            {
                return fs::absolute(Candidate);
            }
        }
        return std::nullopt;
    }

    auto QuoteArgument(const std::wstring_view Argument) -> std::wstring
    {
        if (Argument.empty())
        {
            return L"\"\"";
        }
        if (Argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        {
            return std::wstring{Argument};
        }
        std::wstring Result{L'"'};
        std::size_t Backslashes{};
        for (const auto Character : Argument)
        {
            if (Character == L'\\')
            {
                ++Backslashes;
                continue;
            }
            if (Character == L'"')
            {
                Result.append(Backslashes * 2 + 1, L'\\');
                Result.push_back(L'"');
                Backslashes = 0;
                continue;
            }
            Result.append(Backslashes, L'\\');
            Backslashes = 0;
            Result.push_back(Character);
        }
        Result.append(Backslashes * 2, L'\\');
        Result.push_back(L'"');
        return Result;
    }

    auto BuildCommandLine(const fs::path& Executable,
                          const std::vector<std::wstring>& Arguments) -> std::wstring
    {
        auto Result = QuoteArgument(Executable.wstring());
        for (const auto& Argument : Arguments)
        {
            Result.push_back(L' ');
            Result.append(QuoteArgument(Argument));
        }
        return Result;
    }

    auto Utf8ToWide(const std::string& Value) -> std::wstring
    {
        if (Value.empty())
        {
            return {};
        }
        const auto Required = MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
                static_cast<int>(Value.size()), nullptr, 0);
        if (Required <= 0)
        {
            return std::wstring{Value.begin(), Value.end()};
        }
        std::wstring Result(static_cast<std::size_t>(Required), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
                            static_cast<int>(Value.size()), Result.data(), Required);
        return Result;
    }

    struct FIniDocument
    {
        std::vector<std::string> Lines{};
        std::string Newline{"\r\n"};
        bool Missing{};
        bool HasUtf8Bom{};
        bool EndsWithNewline{};
    };

    struct FSettingsReview
    {
        FIniDocument Document{};
        std::vector<std::string> Issues{};
        std::wstring Error{};
    };

    struct FParsedIniSetting
    {
        std::string_view Key{};
        std::string_view Value{};
    };

    struct FSettingsRepairResult
    {
        bool Success{};
        std::optional<fs::path> Backup{};
        std::wstring Error{};
    };

    auto TrimIniToken(std::string_view Value) -> std::string_view
    {
        constexpr std::string_view Whitespace{" \t\r\n"};
        const auto First = Value.find_first_not_of(Whitespace);
        if (First == std::string_view::npos)
        {
            return {};
        }
        const auto Last = Value.find_last_not_of(Whitespace);
        return Value.substr(First, Last - First + 1);
    }

    auto ParseIniSection(const std::string_view Line)
            -> std::optional<std::string_view>
    {
        const auto Trimmed = TrimIniToken(Line);
        if (Trimmed.size() < 3 || Trimmed.front() != '[' || Trimmed.back() != ']')
        {
            return std::nullopt;
        }
        return TrimIniToken(Trimmed.substr(1, Trimmed.size() - 2));
    }

    auto ParseIniSetting(const std::string_view Line)
            -> std::optional<FParsedIniSetting>
    {
        const auto Trimmed = TrimIniToken(Line);
        if (Trimmed.empty() || Trimmed.front() == ';' || Trimmed.front() == '#')
        {
            return std::nullopt;
        }
        const auto Equals = Trimmed.find('=');
        if (Equals == std::string_view::npos)
        {
            return std::nullopt;
        }
        const auto Key = TrimIniToken(Trimmed.substr(0, Equals));
        if (Key.empty() || Key.front() == ';' || Key.front() == '#')
        {
            return std::nullopt;
        }
        return FParsedIniSetting{
                Key, TrimIniToken(Trimmed.substr(Equals + 1))};
    }

    auto ReadIniDocument(const fs::path& Path) -> FSettingsReview
    {
        FSettingsReview Result{};
        const auto Attributes = GetFileAttributesW(Path.c_str());
        if (Attributes == INVALID_FILE_ATTRIBUTES)
        {
            const auto Error = GetLastError();
            if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND)
            {
                Result.Document.Missing = true;
                Result.Issues.emplace_back("UE4SS-settings.ini is missing");
                return Result;
            }
            Result.Error = L"Unable to inspect UE4SS-settings.ini (Windows error " +
                           std::to_wstring(Error) + L")";
            return Result;
        }
        if ((Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            Result.Error = L"UE4SS-settings.ini is a directory";
            return Result;
        }
        if ((Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            Result.Error = L"Refusing to read or update a reparse-point "
                           L"UE4SS-settings.ini";
            return Result;
        }

        std::ifstream Stream{Path, std::ios::binary};
        if (!Stream)
        {
            Result.Error = L"Unable to open UE4SS-settings.ini for reading";
            return Result;
        }
        const std::string Content{
                std::istreambuf_iterator<char>{Stream}, std::istreambuf_iterator<char>{}};
        if (Stream.bad())
        {
            Result.Error = L"Unable to read UE4SS-settings.ini";
            return Result;
        }
        if (Content.size() >= 2 &&
            ((static_cast<unsigned char>(Content[0]) == 0xFF &&
              static_cast<unsigned char>(Content[1]) == 0xFE) ||
             (static_cast<unsigned char>(Content[0]) == 0xFE &&
              static_cast<unsigned char>(Content[1]) == 0xFF)))
        {
            Result.Error = L"UE4SS-settings.ini uses UTF-16; convert it to UTF-8 before "
                           L"UNBSE can update it";
            return Result;
        }

        std::size_t Cursor{};
        if (Content.size() >= 3 &&
            static_cast<unsigned char>(Content[0]) == 0xEF &&
            static_cast<unsigned char>(Content[1]) == 0xBB &&
            static_cast<unsigned char>(Content[2]) == 0xBF)
        {
            Result.Document.HasUtf8Bom = true;
            Cursor = 3;
        }
        const auto FirstNewline = Content.find('\n', Cursor);
        if (FirstNewline != std::string::npos && FirstNewline > Cursor &&
            Content[FirstNewline - 1] != '\r')
        {
            Result.Document.Newline = "\n";
        }
        Result.Document.EndsWithNewline =
                Content.size() > Cursor && Content.back() == '\n';
        while (Cursor < Content.size())
        {
            const auto End = Content.find('\n', Cursor);
            if (End == std::string::npos)
            {
                auto Line = Content.substr(Cursor);
                if (!Line.empty() && Line.back() == '\r')
                {
                    Line.pop_back();
                }
                Result.Document.Lines.emplace_back(std::move(Line));
                break;
            }
            auto Line = Content.substr(Cursor, End - Cursor);
            if (!Line.empty() && Line.back() == '\r')
            {
                Line.pop_back();
            }
            Result.Document.Lines.emplace_back(std::move(Line));
            Cursor = End + 1;
        }

        for (const auto& Required : RequiredIniSettings)
        {
            std::string_view CurrentSection{};
            bool Found{};
            std::optional<std::string> WrongValue{};
            for (const auto& Line : Result.Document.Lines)
            {
                if (const auto Section = ParseIniSection(Line))
                {
                    CurrentSection = *Section;
                    continue;
                }
                const auto Setting = ParseIniSetting(Line);
                if (!Setting || CurrentSection != Required.Section ||
                    Setting->Key != Required.Key)
                {
                    continue;
                }
                Found = true;
                if (!IsIniSettingValueValid(Required, Setting->Value))
                {
                    WrongValue = std::string{Setting->Value};
                }
            }
            if (!Found)
            {
                Result.Issues.emplace_back(
                        "[" + std::string{Required.Section} + "] " +
                        std::string{Required.Key} + " is missing (" +
                        (IsDefaultIniSetting(Required) ? "default " : "required ") +
                        std::string{Required.Value} + ")");
            }
            else if (WrongValue)
            {
                Result.Issues.emplace_back(
                        "[" + std::string{Required.Section} + "] " +
                        std::string{Required.Key} + " is '" + *WrongValue +
                        "' (" + IniSettingExpectation(Required) + ")");
            }
        }
        return Result;
    }

    auto UnbseToggleEnabled(const FIniDocument& Document,
                            const std::string_view Key) -> bool
    {
        std::string_view CurrentSection{};
        bool Enabled{};
        for (const auto& Line : Document.Lines)
        {
            if (const auto Section = ParseIniSection(Line))
            {
                CurrentSection = *Section;
                continue;
            }
            const auto Setting = ParseIniSetting(Line);
            if (Setting && CurrentSection == "UNBSE" &&
                Setting->Key == Key)
            {
                Enabled = Setting->Value == "1";
            }
        }
        return Enabled;
    }

    auto ApplyRequiredIniSetting(FIniDocument& Document,
                                 const FRequiredIniSetting& Required) -> void
    {
        std::string CurrentSection{};
        std::vector<std::size_t> Matches{};
        std::optional<std::size_t> FirstSectionHeader{};
        std::optional<std::size_t> FirstSectionEnd{};
        bool InFirstSection{};
        for (std::size_t Index = 0; Index < Document.Lines.size(); ++Index)
        {
            if (const auto Section = ParseIniSection(Document.Lines[Index]))
            {
                if (InFirstSection && !FirstSectionEnd)
                {
                    FirstSectionEnd = Index;
                }
                CurrentSection = std::string{*Section};
                if (CurrentSection == Required.Section && !FirstSectionHeader)
                {
                    FirstSectionHeader = Index;
                    InFirstSection = true;
                }
                else
                {
                    InFirstSection = false;
                }
                continue;
            }
            const auto Setting = ParseIniSetting(Document.Lines[Index]);
            if (Setting && CurrentSection == Required.Section &&
                Setting->Key == Required.Key)
            {
                Matches.emplace_back(Index);
            }
        }
        if (InFirstSection && !FirstSectionEnd)
        {
            FirstSectionEnd = Document.Lines.size();
        }

        const auto Replacement = std::string{Required.Key} + " = " +
                                 std::string{Required.Value};
        if (!Matches.empty())
        {
            for (const auto Index : Matches)
            {
                const auto Setting = ParseIniSetting(Document.Lines[Index]);
                if (!Setting || !IsIniSettingValueValid(Required, Setting->Value))
                {
                    Document.Lines[Index] = Replacement;
                }
            }
            return;
        }
        if (FirstSectionHeader)
        {
            Document.Lines.insert(
                    Document.Lines.begin() + static_cast<std::ptrdiff_t>(
                                                     FirstSectionEnd.value_or(
                                                             Document.Lines.size())),
                    Replacement);
            return;
        }
        if (!Document.Lines.empty() && !Document.Lines.back().empty())
        {
            Document.Lines.emplace_back();
        }
        Document.Lines.emplace_back("[" + std::string{Required.Section} + "]");
        Document.Lines.emplace_back(Replacement);
    }

    auto SerializeIniDocument(FIniDocument Document) -> std::string
    {
        for (const auto& Required : RequiredIniSettings)
        {
            ApplyRequiredIniSetting(Document, Required);
        }
        constexpr std::string_view SettingsProfilePrefix =
                "; UNBSE-Settings-Profile:";
        bool HasSettingsProfile{};
        for (auto Line = Document.Lines.begin(); Line != Document.Lines.end();)
        {
            if (!TrimIniToken(*Line).starts_with(SettingsProfilePrefix))
            {
                ++Line;
                continue;
            }
            if (!HasSettingsProfile)
            {
                *Line = SettingsProfileMarker;
                HasSettingsProfile = true;
                ++Line;
            }
            else
            {
                Line = Document.Lines.erase(Line);
            }
        }
        if (!HasSettingsProfile)
        {
            Document.Lines.insert(Document.Lines.begin(),
                                  std::string{SettingsProfileMarker});
        }
        Document.EndsWithNewline = true;

        std::string Result{};
        if (Document.HasUtf8Bom)
        {
            Result.append("\xEF\xBB\xBF", 3);
        }
        for (std::size_t Index = 0; Index < Document.Lines.size(); ++Index)
        {
            Result.append(Document.Lines[Index]);
            if (Index + 1 < Document.Lines.size() || Document.EndsWithNewline)
            {
                Result.append(Document.Newline);
            }
        }
        return Result;
    }

    auto UniqueSiblingPath(const fs::path& Path, const std::wstring_view Suffix)
            -> std::optional<fs::path>
    {
        for (std::size_t Index = 0; Index < 1000; ++Index)
        {
            auto Candidate = Path;
            Candidate += Suffix;
            if (Index != 0)
            {
                Candidate += L"-" + std::to_wstring(Index);
            }
            std::error_code Error{};
            if (!fs::exists(Candidate, Error) && !Error)
            {
                return Candidate;
            }
        }
        return std::nullopt;
    }

    auto RepairSettingsFile(const fs::path& Path, const FIniDocument& Document)
            -> FSettingsRepairResult
    {
        FSettingsRepairResult Result{};
        const auto Temporary = UniqueSiblingPath(Path, L".unbse-tmp");
        if (!Temporary)
        {
            Result.Error = L"Unable to reserve a temporary settings path";
            return Result;
        }
        const auto Bytes = SerializeIniDocument(Document);
        {
            std::ofstream Stream{*Temporary, std::ios::binary | std::ios::trunc};
            if (!Stream)
            {
                Result.Error = L"Unable to create the temporary settings file";
                return Result;
            }
            Stream.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
            Stream.flush();
            if (!Stream)
            {
                Stream.close();
                std::error_code CleanupError{};
                fs::remove(*Temporary, CleanupError);
                Result.Error = L"Unable to write the temporary settings file";
                return Result;
            }
        }

        if (Document.Missing)
        {
            if (!MoveFileExW(Temporary->c_str(), Path.c_str(), MOVEFILE_WRITE_THROUGH))
            {
                const auto Error = GetLastError();
                std::error_code CleanupError{};
                fs::remove(*Temporary, CleanupError);
                Result.Error = L"Unable to create UE4SS-settings.ini (Windows error " +
                               std::to_wstring(Error) + L")";
                return Result;
            }
        }
        else
        {
            Result.Backup = UniqueSiblingPath(Path, L".unbse-backup");
            if (!Result.Backup)
            {
                std::error_code CleanupError{};
                fs::remove(*Temporary, CleanupError);
                Result.Error = L"Unable to reserve a settings backup path";
                return Result;
            }
            if (!ReplaceFileW(Path.c_str(), Temporary->c_str(),
                              Result.Backup->c_str(), REPLACEFILE_WRITE_THROUGH,
                              nullptr, nullptr))
            {
                const auto Error = GetLastError();
                std::error_code CleanupError{};
                fs::remove(*Temporary, CleanupError);
                Result.Error = L"Unable to replace UE4SS-settings.ini (Windows error " +
                               std::to_wstring(Error) + L")";
                Result.Backup.reset();
                return Result;
            }
        }
        Result.Success = true;
        return Result;
    }

    auto EnsureCompatibleSettings(const fs::path& Path, const bool ValidateOnly,
                                  bool& Repaired,
                                  bool& EnablePluginVersionWarning,
                                  bool& EnableAssetContainerWarning) -> bool
    {
        Repaired = false;
        EnablePluginVersionWarning = false;
        EnableAssetContainerWarning = false;
        const auto Review = ReadIniDocument(Path);
        if (!Review.Error.empty())
        {
            std::wcerr << L"ERROR: " << Review.Error << L": " << Path << L"\n";
            return false;
        }
        if (Review.Issues.empty())
        {
            EnablePluginVersionWarning =
                    UnbseToggleEnabled(Review.Document,
                                        "EnablePluginVersionWarning");
            EnableAssetContainerWarning =
                    UnbseToggleEnabled(Review.Document,
                                        "EnableAssetContainerWarning");
            return true;
        }

        std::wcerr << L"ERROR: UE4SS-settings.ini is not UNBSE 0.14.5 compliant:\n";
        for (const auto& Issue : Review.Issues)
        {
            std::wcerr << L"  - " << Utf8ToWide(Issue) << L"\n";
        }
        if (ValidateOnly)
        {
            std::wcerr << L"Run UNBSELoader.exe normally to review and approve a "
                          L"settings repair. No files were changed.\n";
            return false;
        }

        std::wostringstream Prompt{};
        Prompt << L"UE4SS-settings.ini is not compatible with UNBSE 0.14.5.\n\n";
        constexpr std::size_t MaximumDisplayedIssues = 15;
        const auto Displayed = std::min(Review.Issues.size(), MaximumDisplayedIssues);
        for (std::size_t Index = 0; Index < Displayed; ++Index)
        {
            Prompt << L"- " << Utf8ToWide(Review.Issues[Index]) << L"\n";
        }
        if (Review.Issues.size() > Displayed)
        {
            Prompt << L"...and " << (Review.Issues.size() - Displayed)
                   << L" more issue(s).\n";
        }
        Prompt << L"\nUpdate the INI now? Only UNBSE's managed required/default keys "
                  L"will be added or corrected. Other settings and comments are "
                  L"preserved.";
        if (!Review.Document.Missing)
        {
            Prompt << L" A uniquely named backup will be created first.";
        }
        if (MessageBoxW(nullptr, Prompt.str().c_str(),
                        L"UNBSE - Update UE4SS Settings?",
                        MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2 |
                                MB_SETFOREGROUND) != IDYES)
        {
            std::wcerr << L"ERROR: Settings repair was declined; launch cancelled.\n";
            return false;
        }

        const auto Repair = RepairSettingsFile(Path, Review.Document);
        if (!Repair.Success)
        {
            std::wcerr << L"ERROR: " << Repair.Error << L"\n";
            MessageBoxW(nullptr, Repair.Error.c_str(), L"UNBSE Settings Repair Failed",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return false;
        }
        const auto Recheck = ReadIniDocument(Path);
        if (!Recheck.Error.empty() || !Recheck.Issues.empty())
        {
            std::wcerr << L"ERROR: UE4SS-settings.ini remained incompatible after "
                          L"the repair; launch cancelled.\n";
            return false;
        }
        std::wcout << L"Updated UNBSE-required UE4SS settings: " << Path << L"\n";
        if (Repair.Backup)
        {
            std::wcout << L"Settings backup: " << *Repair.Backup << L"\n";
        }
        Repaired = true;
        return true;
    }

    auto FormatRuntimeVersion(const std::uint32_t Version) -> std::wstring
    {
        return std::to_wstring((Version >> 28u) & 0xFu) + L"." +
               std::to_wstring((Version >> 16u) & 0xFFFu) + L"." +
               std::to_wstring((Version >> 4u) & 0xFFFu) + L"." +
               std::to_wstring(Version & 0xFu);
    }

    struct FPluginPreflightWarning
    {
        std::wstring Name{};
        std::wstring Author{};
        fs::path Path{};
        std::wstring Reason{};
        std::wstring DeclaredVersions{};
        std::wstring Fingerprint{};
    };

    auto DeclaredRuntimeVersions(
            const RC::UNBSE::OBSE64::FPluginScanResult& Plugin) -> std::wstring
    {
        std::wstring Result{};
        for (const auto Version : Plugin.Version.CompatibleVersions)
        {
            if (Version == 0)
            {
                continue;
            }
            if (!Result.empty())
            {
                Result.append(L", ");
            }
            Result.append(FormatRuntimeVersion(Version));
        }
        return Result.empty() ? L"none" : Result;
    }

    auto PluginExplicitlySupportsRuntime(
            const RC::UNBSE::OBSE64::FPluginScanResult& Plugin,
            const std::uint32_t RuntimeVersion) -> bool
    {
        return Plugin.Status == RC::UNBSE::OBSE64::EPluginScanStatus::Ok &&
               RuntimeVersion != 0 &&
               std::find(Plugin.Version.CompatibleVersions.begin(),
                         Plugin.Version.CompatibleVersions.end(), RuntimeVersion) !=
                       Plugin.Version.CompatibleVersions.end();
    }

    auto ReviewPluginVersions(const fs::path& GameDirectory)
            -> std::vector<FPluginPreflightWarning>
    {
        namespace OBSE64 = RC::UNBSE::OBSE64;
        const auto PluginDirectory = GameDirectory / L"OBSE" / L"Plugins";
        std::vector<fs::path> Paths{};
        std::error_code Error{};
        if (!fs::exists(PluginDirectory, Error) && !Error)
        {
            return {};
        }
        for (fs::directory_iterator Iterator{PluginDirectory, Error}, End;
             !Error && Iterator != End; Iterator.increment(Error))
        {
            if (!Iterator->is_regular_file(Error))
            {
                continue;
            }
            auto Extension = ToLower(Iterator->path().extension().wstring());
            if (Extension == L".dll")
            {
                Paths.push_back(Iterator->path());
            }
        }
        if (Error)
        {
            return {{L"OBSE plugin directory", L"unknown", PluginDirectory,
                     L"could not be enumerated safely (Windows error " +
                             std::to_wstring(Error.value()) + L")",
                     L"unknown", L""}};
        }
        std::sort(Paths.begin(), Paths.end());

        std::vector<FPluginPreflightWarning> Warnings{};
        for (const auto& Path : Paths)
        {
            const auto Plugin = OBSE64::ScanPlugin(Path);
            if (Plugin.Status != OBSE64::EPluginScanStatus::Ok)
            {
                Warnings.push_back(
                        {Path.stem().wstring(), L"unknown", Path,
                         L"does not contain a readable OBSE64 v1 declaration (" +
                                 Utf8ToWide(OBSE64::PluginScanStatusName(Plugin.Status)) +
                                 L")",
                         L"unknown", Sha256File(Path)});
                continue;
            }

            std::wstring Reason{};
            if (!Plugin.HasLoad)
            {
                Reason = L"does not export the required OBSEPlugin_Load entry point";
            }
            else if (!PluginExplicitlySupportsRuntime(Plugin, SupportedRuntimeVersion))
            {
                const auto HasDeclaredVersion = std::any_of(
                        Plugin.Version.CompatibleVersions.begin(),
                        Plugin.Version.CompatibleVersions.end(),
                        [](const std::uint32_t Version) { return Version != 0; });
                if (HasDeclaredVersion)
                {
                    Reason = L"has a version-specific declaration that excludes game version ";
                }
                else if ((Plugin.Version.AddressIndependence &
                          OBSE64::AddressIndependenceSignatures) != 0)
                {
                    Reason = L"claims signature-based version independence but does not "
                             L"explicitly declare game version ";
                }
                else if ((Plugin.Version.AddressIndependence &
                          OBSE64::AddressIndependenceAddressLibrary) != 0)
                {
                    Reason = L"claims address-library version independence but does not "
                             L"explicitly declare game version ";
                }
                else
                {
                    Reason = L"does not explicitly declare support for game version ";
                }
                Reason.append(FormatRuntimeVersion(SupportedRuntimeVersion));
            }
            if (Reason.empty())
            {
                continue;
            }
            Warnings.push_back(
                    {Utf8ToWide(Plugin.Name),
                     Plugin.Author.empty() ? L"unknown" : Utf8ToWide(Plugin.Author), Path,
                     std::move(Reason),
                     DeclaredRuntimeVersions(Plugin), Sha256File(Path)});
        }
        return Warnings;
    }

    auto WarningAcceptancePath() -> std::optional<fs::path>
    {
        const auto Required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (Required == 0)
        {
            return std::nullopt;
        }
        std::vector<wchar_t> Buffer(Required);
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", Buffer.data(), Required) == 0)
        {
            return std::nullopt;
        }
        return fs::path{Buffer.data()} / L"UNBSE" /
               L"accepted-plugin-version-warnings-v1.txt";
    }

    auto LoadAcceptedPluginWarnings(const std::wstring_view RuntimeHash)
            -> std::unordered_set<std::wstring>
    {
        std::unordered_set<std::wstring> Result{};
        const auto Path = WarningAcceptancePath();
        if (!Path)
        {
            return Result;
        }
        std::wifstream Input{*Path};
        std::wstring RecordedRuntimeHash{};
        std::wstring PluginHash{};
        while (Input >> RecordedRuntimeHash >> PluginHash)
        {
            if (RecordedRuntimeHash == RuntimeHash)
            {
                Result.insert(std::move(PluginHash));
            }
        }
        return Result;
    }

    auto PendingPluginWarnings(const std::vector<FPluginPreflightWarning>& Warnings,
                               const std::wstring_view RuntimeHash)
            -> std::vector<FPluginPreflightWarning>
    {
        const auto Accepted = LoadAcceptedPluginWarnings(RuntimeHash);
        std::vector<FPluginPreflightWarning> Result{};
        std::copy_if(Warnings.begin(), Warnings.end(), std::back_inserter(Result),
                     [&Accepted](const FPluginPreflightWarning& Warning) {
                         return Warning.Fingerprint.empty() ||
                                Accepted.find(Warning.Fingerprint) == Accepted.end();
                     });
        return Result;
    }

    auto RememberPluginWarnings(
            const std::vector<FPluginPreflightWarning>& Warnings,
            const std::wstring_view RuntimeHash) -> void
    {
        if (Warnings.empty())
        {
            return;
        }
        const auto Path = WarningAcceptancePath();
        if (!Path)
        {
            std::wcerr << L"WARNING: LOCALAPPDATA is unavailable; plugin warning choices "
                          L"cannot be remembered.\n";
            return;
        }
        auto Accepted = LoadAcceptedPluginWarnings(RuntimeHash);
        for (const auto& Warning : Warnings)
        {
            if (!Warning.Fingerprint.empty())
            {
                Accepted.insert(Warning.Fingerprint);
            }
        }
        std::error_code Error{};
        fs::create_directories(Path->parent_path(), Error);
        if (Error)
        {
            std::wcerr << L"WARNING: Unable to create the UNBSE warning-state directory.\n";
            return;
        }
        std::wofstream Output{*Path, std::ios::trunc};
        if (!Output)
        {
            std::wcerr << L"WARNING: Unable to remember plugin warning choices.\n";
            return;
        }
        for (const auto& Fingerprint : Accepted)
        {
            Output << RuntimeHash << L' ' << Fingerprint << L'\n';
        }
    }

    auto PrintPluginVersionWarnings(
            const std::vector<FPluginPreflightWarning>& Warnings) -> void
    {
        for (const auto& Warning : Warnings)
        {
            std::wcerr << L"WARNING: Invalid Version Mod: \"" << Warning.Name << L"\"\n"
                       << L"  Author: " << Warning.Author << L"\n"
                       << L"  " << Warning.Reason << L". This may cause crashes.\n"
                       << L"  File: " << Warning.Path << L"\n"
                       << L"  Declared game versions: " << Warning.DeclaredVersions << L"\n";
        }
    }

    struct FScrollableWarningDialogState
    {
        bool Complete{};
        bool Accepted{};
        HWND Summary{};
        HWND Details{};
        HWND Accept{};
        HWND Reject{};
    };

    auto LayoutScrollableWarningDialog(const HWND Window,
                                       const FScrollableWarningDialogState& State) -> void
    {
        RECT Client{};
        if (!GetClientRect(Window, &Client))
        {
            return;
        }
        constexpr int Margin = 16;
        constexpr int Gap = 12;
        constexpr int SummaryHeight = 72;
        constexpr int ButtonWidth = 132;
        constexpr int ButtonHeight = 32;
        const int Width = static_cast<int>(Client.right - Client.left);
        const int Height = static_cast<int>(Client.bottom - Client.top);
        const auto ButtonsTop = Height - Margin - ButtonHeight;
        const auto DetailsTop = Margin + SummaryHeight + Gap;
        const auto DetailsHeight = std::max(80, ButtonsTop - Gap - DetailsTop);
        MoveWindow(State.Summary, Margin, Margin, std::max(1, Width - (2 * Margin)),
                   SummaryHeight, TRUE);
        MoveWindow(State.Details, Margin, DetailsTop,
                   std::max(1, Width - (2 * Margin)), DetailsHeight, TRUE);
        MoveWindow(State.Reject, Width - Margin - ButtonWidth, ButtonsTop,
                   ButtonWidth, ButtonHeight, TRUE);
        MoveWindow(State.Accept, Width - Margin - (2 * ButtonWidth) - Gap,
                   ButtonsTop, ButtonWidth, ButtonHeight, TRUE);
    }

    auto CALLBACK ScrollableWarningDialogProc(const HWND Window, const UINT Message,
                                              const WPARAM WParam,
                                              const LPARAM LParam) -> LRESULT
    {
        auto* State = reinterpret_cast<FScrollableWarningDialogState*>(
                GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE)
        {
            const auto* Create = reinterpret_cast<const CREATESTRUCTW*>(LParam);
            State = static_cast<FScrollableWarningDialogState*>(Create->lpCreateParams);
            SetWindowLongPtrW(Window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(State));
        }
        if (!State)
        {
            return DefWindowProcW(Window, Message, WParam, LParam);
        }
        switch (Message)
        {
        case WM_SIZE:
            LayoutScrollableWarningDialog(Window, *State);
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* Size = reinterpret_cast<MINMAXINFO*>(LParam);
            Size->ptMinTrackSize.x = 560;
            Size->ptMinTrackSize.y = 380;
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(WParam) == IDYES || LOWORD(WParam) == IDNO)
            {
                State->Accepted = LOWORD(WParam) == IDYES;
                State->Complete = true;
                DestroyWindow(Window);
                return 0;
            }
            break;
        case WM_CLOSE:
            State->Accepted = false;
            State->Complete = true;
            DestroyWindow(Window);
            return 0;
        case WM_NCDESTROY:
            State->Complete = true;
            SetWindowLongPtrW(Window, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return DefWindowProcW(Window, Message, WParam, LParam);
    }

    auto ShowScrollableWarningDialog(const std::wstring_view Title,
                                     const std::wstring_view Summary,
                                     const std::wstring_view Details) -> bool
    {
        constexpr auto WindowClassName = L"UNBSEScrollableWarningDialog";
        const auto Instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW WindowClass{};
        WindowClass.cbSize = sizeof(WindowClass);
        WindowClass.style = CS_HREDRAW | CS_VREDRAW;
        WindowClass.lpfnWndProc = ScrollableWarningDialogProc;
        WindowClass.hInstance = Instance;
        WindowClass.hIcon = LoadIconW(nullptr, IDI_WARNING);
        WindowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        WindowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        WindowClass.lpszClassName = WindowClassName;
        if (!RegisterClassExW(&WindowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return MessageBoxW(nullptr, std::wstring{Summary}.c_str(),
                               std::wstring{Title}.c_str(),
                               MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2 |
                                       MB_SETFOREGROUND) == IDYES;
        }

        RECT WorkArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &WorkArea, 0);
        const int WorkWidth = static_cast<int>(WorkArea.right - WorkArea.left);
        const int WorkHeight = static_cast<int>(WorkArea.bottom - WorkArea.top);
        const auto Width = std::min(780, WorkWidth - 40);
        const auto Height = std::min(560, WorkHeight - 40);
        const auto Left = WorkArea.left +
                          ((WorkArea.right - WorkArea.left - Width) / 2);
        const auto Top = WorkArea.top +
                         ((WorkArea.bottom - WorkArea.top - Height) / 2);
        FScrollableWarningDialogState State{};
        const auto Window = CreateWindowExW(
                WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT | WS_EX_TOPMOST,
                WindowClassName, std::wstring{Title}.c_str(),
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
                        WS_CLIPCHILDREN,
                Left, Top, Width, Height, nullptr, nullptr, Instance, &State);
        if (!Window)
        {
            return MessageBoxW(nullptr, std::wstring{Summary}.c_str(),
                               std::wstring{Title}.c_str(),
                               MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2 |
                                       MB_SETFOREGROUND) == IDYES;
        }

        State.Summary = CreateWindowExW(
                0, L"STATIC", std::wstring{Summary}.c_str(), WS_CHILD | WS_VISIBLE,
                0, 0, 0, 0, Window, nullptr, Instance, nullptr);
        State.Details = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                        ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL,
                0, 0, 0, 0, Window, nullptr, Instance, nullptr);
        State.Accept = CreateWindowExW(
                0, L"BUTTON", L"Launch Anyway", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 0, 0, Window, reinterpret_cast<HMENU>(IDYES), Instance,
                nullptr);
        State.Reject = CreateWindowExW(
                0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0,
                Window, reinterpret_cast<HMENU>(IDNO), Instance, nullptr);
        if (!State.Summary || !State.Details || !State.Accept || !State.Reject)
        {
            DestroyWindow(Window);
            return false;
        }

        const auto Font = GetStockObject(DEFAULT_GUI_FONT);
        for (const auto Control :
             {State.Summary, State.Details, State.Accept, State.Reject})
        {
            SendMessageW(Control, WM_SETFONT, reinterpret_cast<WPARAM>(Font), TRUE);
        }
        SendMessageW(State.Details, EM_SETLIMITTEXT,
                     static_cast<WPARAM>(std::min<std::size_t>(
                             Details.size() + 1,
                             static_cast<std::size_t>(std::numeric_limits<LONG>::max()))),
                     0);
        SetWindowTextW(State.Details, std::wstring{Details}.c_str());
        LayoutScrollableWarningDialog(Window, State);
        ShowWindow(Window, SW_SHOW);
        UpdateWindow(Window);
        SetForegroundWindow(Window);
        SetFocus(State.Reject);

        MSG Message{};
        while (!State.Complete)
        {
            const auto Status = GetMessageW(&Message, nullptr, 0, 0);
            if (Status <= 0)
            {
                State.Complete = true;
                break;
            }
            if (!IsDialogMessageW(Window, &Message))
            {
                TranslateMessage(&Message);
                DispatchMessageW(&Message);
            }
        }
        if (IsWindow(Window))
        {
            DestroyWindow(Window);
        }
        return State.Accepted;
    }

    auto ConfirmPluginVersionWarnings(
            const std::vector<FPluginPreflightWarning>& Warnings) -> bool
    {
        if (Warnings.empty())
        {
            return true;
        }
        std::wostringstream Summary{};
        Summary << Warnings.size()
                << L" native plugin(s) declare an incompatible or unknown game version.\n"
                   L"Review the scrollable list, then choose whether to launch. Cancel is "
                   L"the safe default.";
        std::wostringstream Details{};
        for (const auto& Warning : Warnings)
        {
            Details << L"Invalid Version Mod: \"" << Warning.Name << L"\"\r\n"
                    << L"Author: " << Warning.Author << L"\r\n"
                    << Warning.Reason << L". This may cause crashes.\r\n"
                    << L"File: " << Warning.Path << L"\r\n"
                    << L"Declared game versions: " << Warning.DeclaredVersions
                    << L"\r\n\r\n";
        }
        return ShowScrollableWarningDialog(
                L"UNBSE - Invalid Version Mod Warning", Summary.str(), Details.str());
    }

    struct FPendingAssetWarning
    {
        RC::UNBSE::Preflight::FAssetContainerWarning Warning{};
        std::wstring Fingerprint{};
    };

    auto AssetWarningAcceptancePath() -> std::optional<fs::path>
    {
        auto Path = WarningAcceptancePath();
        if (Path)
        {
            Path->replace_filename(L"accepted-asset-container-warnings-v1.txt");
        }
        return Path;
    }

    auto AssetWarningFingerprint(
            const RC::UNBSE::Preflight::FAssetContainerWarning& Warning) -> std::wstring
    {
        if (Warning.FingerprintPrimaryFile)
        {
            try
            {
                return Sha256File(Warning.Path);
            }
            catch (const std::exception&)
            {
            }
        }
        return Sha256Text(Warning.FingerprintMaterial);
    }

    auto LoadAcceptedAssetWarnings(const std::wstring_view RuntimeHash)
            -> std::unordered_set<std::wstring>
    {
        std::unordered_set<std::wstring> Result{};
        const auto Path = AssetWarningAcceptancePath();
        if (!Path)
        {
            return Result;
        }
        std::wifstream Input{*Path};
        std::wstring RecordedRuntimeHash{};
        std::wstring WarningHash{};
        while (Input >> RecordedRuntimeHash >> WarningHash)
        {
            if (RecordedRuntimeHash == RuntimeHash)
            {
                Result.insert(std::move(WarningHash));
            }
        }
        return Result;
    }

    auto PendingAssetWarnings(
            const std::vector<RC::UNBSE::Preflight::FAssetContainerWarning>& Warnings,
            const std::wstring_view RuntimeHash) -> std::vector<FPendingAssetWarning>
    {
        const auto Accepted = LoadAcceptedAssetWarnings(RuntimeHash);
        std::vector<FPendingAssetWarning> Result{};
        for (const auto& Warning : Warnings)
        {
            auto Fingerprint = AssetWarningFingerprint(Warning);
            if (Accepted.find(Fingerprint) == Accepted.end())
            {
                Result.push_back({Warning, std::move(Fingerprint)});
            }
        }
        return Result;
    }

    auto RememberAssetWarnings(const std::vector<FPendingAssetWarning>& Warnings,
                               const std::wstring_view RuntimeHash) -> void
    {
        if (Warnings.empty())
        {
            return;
        }
        const auto Path = AssetWarningAcceptancePath();
        if (!Path)
        {
            std::wcerr << L"WARNING: LOCALAPPDATA is unavailable; asset warning choices "
                          L"cannot be remembered.\n";
            return;
        }
        auto Accepted = LoadAcceptedAssetWarnings(RuntimeHash);
        for (const auto& Warning : Warnings)
        {
            Accepted.insert(Warning.Fingerprint);
        }
        std::error_code Error{};
        fs::create_directories(Path->parent_path(), Error);
        if (Error)
        {
            std::wcerr << L"WARNING: Unable to create the UNBSE warning-state directory.\n";
            return;
        }
        std::wofstream Output{*Path, std::ios::trunc};
        if (!Output)
        {
            std::wcerr << L"WARNING: Unable to remember asset warning choices.\n";
            return;
        }
        for (const auto& Fingerprint : Accepted)
        {
            Output << RuntimeHash << L' ' << Fingerprint << L'\n';
        }
    }

    auto PrintAssetWarnings(
            const std::vector<RC::UNBSE::Preflight::FAssetContainerWarning>& Warnings)
            -> void
    {
        for (const auto& Warning : Warnings)
        {
            std::wcerr << L"WARNING: Invalid Asset Container: \"" << Warning.Name << L"\"\n"
                       << L"  " << Warning.Reason
                       << L". This may prevent mounting or cause deserialization crashes.\n"
                       << L"  File: " << Warning.Path << L"\n"
                       << L"  Evidence: " << Warning.Evidence << L"\n";
        }
    }

    auto PrintPendingAssetWarnings(const std::vector<FPendingAssetWarning>& Warnings)
            -> void
    {
        std::vector<RC::UNBSE::Preflight::FAssetContainerWarning> Values{};
        Values.reserve(Warnings.size());
        std::transform(Warnings.begin(), Warnings.end(), std::back_inserter(Values),
                       [](const FPendingAssetWarning& Warning) {
                           return Warning.Warning;
                       });
        PrintAssetWarnings(Values);
    }

    auto ConfirmAssetWarnings(const std::vector<FPendingAssetWarning>& Warnings) -> bool
    {
        if (Warnings.empty())
        {
            return true;
        }
        std::wostringstream Summary{};
        Summary << Warnings.size()
                << L" asset container(s) differ from the verified retail format.\n"
                   L"Review the scrollable evidence, then choose whether to launch. Cancel "
                   L"is the safe default.";
        std::wostringstream Details{};
        for (const auto& Pending : Warnings)
        {
            const auto& Warning = Pending.Warning;
            Details << L"Invalid Asset Container: \"" << Warning.Name << L"\"\r\n"
                    << Warning.Reason
                    << L". This may prevent mounting or cause deserialization crashes.\r\n"
                    << L"File: " << Warning.Path << L"\r\n"
                    << L"Evidence: " << Warning.Evidence << L"\r\n\r\n";
        }
        return ShowScrollableWarningDialog(
                L"UNBSE - Asset Container Compatibility Warning", Summary.str(),
                Details.str());
    }

    auto FindRemoteModule(const DWORD ProcessId, const std::wstring_view ModuleName)
            -> std::optional<std::uintptr_t>
    {
        for (int Attempt = 0; Attempt < 8; ++Attempt)
        {
            FHandle Snapshot{CreateToolhelp32Snapshot(
                    TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, ProcessId)};
            if (!Snapshot && GetLastError() == ERROR_BAD_LENGTH)
            {
                continue;
            }
            if (!Snapshot)
            {
                return std::nullopt;
            }
            MODULEENTRY32W Entry{};
            Entry.dwSize = sizeof(Entry);
            if (!Module32FirstW(Snapshot.get(), &Entry))
            {
                return std::nullopt;
            }
            const auto Expected = ToLower(std::wstring{ModuleName});
            do
            {
                if (ToLower(Entry.szModule) == Expected)
                {
                    return reinterpret_cast<std::uintptr_t>(Entry.modBaseAddr);
                }
            } while (Module32NextW(Snapshot.get(), &Entry));
            return std::nullopt;
        }
        return std::nullopt;
    }

    auto WaitForRemoteModule(const HANDLE Process, const DWORD ProcessId,
                             const std::wstring_view ModuleName,
                             const std::chrono::milliseconds Timeout) -> bool
    {
        const auto Deadline = std::chrono::steady_clock::now() + Timeout;
        while (std::chrono::steady_clock::now() < Deadline)
        {
            if (FindRemoteModule(ProcessId, ModuleName))
            {
                return true;
            }
            if (WaitForSingleObject(Process, 0) == WAIT_OBJECT_0)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }
        return false;
    }

    auto InjectLibrary(const HANDLE Process, const DWORD ProcessId,
                       const fs::path& Library) -> bool
    {
        const auto Kernel32 = GetModuleHandleW(L"kernel32.dll");
        const auto LoadLibrary = Kernel32 ? GetProcAddress(Kernel32, "LoadLibraryW") : nullptr;
        MEMORY_BASIC_INFORMATION Memory{};
        if (!LoadLibrary || VirtualQuery(LoadLibrary, &Memory, sizeof(Memory)) != sizeof(Memory))
        {
            std::wcerr << L"ERROR: Unable to resolve local LoadLibraryW.\n";
            return false;
        }
        const auto LocalOwner = static_cast<HMODULE>(Memory.AllocationBase);
        std::array<wchar_t, MAX_PATH> OwnerName{};
        if (GetModuleBaseNameW(GetCurrentProcess(), LocalOwner, OwnerName.data(),
                               static_cast<DWORD>(OwnerName.size())) == 0)
        {
            std::wcerr << L"ERROR: Unable to identify the LoadLibraryW owner module.\n";
            return false;
        }
        if (!WaitForRemoteModule(Process, ProcessId, OwnerName.data(),
                                 std::chrono::seconds{10}))
        {
            std::wcerr << L"ERROR: Child process did not initialize " << OwnerName.data()
                       << L" before the injection deadline.\n";
            return false;
        }
        const auto RemoteOwner = FindRemoteModule(ProcessId, OwnerName.data());
        if (!RemoteOwner)
        {
            std::wcerr << L"ERROR: Child process has no " << OwnerName.data() << L" module.\n";
            return false;
        }
        const auto LocalOwnerAddress = reinterpret_cast<std::uintptr_t>(LocalOwner);
        const auto LocalLoadLibraryAddress = reinterpret_cast<std::uintptr_t>(LoadLibrary);
        const auto RemoteLoadLibrary = *RemoteOwner +
                                       (LocalLoadLibraryAddress - LocalOwnerAddress);

        const auto LibraryText = Library.wstring();
        const auto Bytes = (LibraryText.size() + 1) * sizeof(wchar_t);
        auto* RemoteText = VirtualAllocEx(Process, nullptr, Bytes, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
        if (!RemoteText)
        {
            std::wcerr << L"ERROR: VirtualAllocEx failed: "
                       << FormatWindowsError(GetLastError()) << L"\n";
            return false;
        }
        struct FRemoteMemory
        {
            HANDLE Process{};
            void* Address{};
            ~FRemoteMemory()
            {
                if (Address)
                {
                    VirtualFreeEx(Process, Address, 0, MEM_RELEASE);
                }
            }
        } RemoteMemory{Process, RemoteText};

        SIZE_T Written{};
        if (!WriteProcessMemory(Process, RemoteText, LibraryText.c_str(), Bytes, &Written) ||
            Written != Bytes)
        {
            std::wcerr << L"ERROR: WriteProcessMemory failed: "
                       << FormatWindowsError(GetLastError()) << L"\n";
            return false;
        }

        FHandle Thread{CreateRemoteThread(
                Process, nullptr, 0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(RemoteLoadLibrary), RemoteText, 0,
                nullptr)};
        if (!Thread)
        {
            std::wcerr << L"ERROR: CreateRemoteThread failed: "
                       << FormatWindowsError(GetLastError()) << L"\n";
            return false;
        }
        if (WaitForSingleObject(Thread.get(), 30000) != WAIT_OBJECT_0)
        {
            std::wcerr << L"ERROR: Timed out while loading UE4SS in the game process.\n";
            return false;
        }
        DWORD Result{};
        if (!GetExitCodeThread(Thread.get(), &Result))
        {
            std::wcerr << L"ERROR: GetExitCodeThread failed after loading UE4SS: "
                       << FormatWindowsError(GetLastError()) << L"\n";
            return false;
        }
        // A remote thread exit code is only a DWORD, while LoadLibraryW returns a
        // 64-bit module handle. The truncated value can be zero for a successful
        // x64 load, so verify the durable remote module state instead.
        if (!WaitForRemoteModule(Process, ProcessId, Library.filename().wstring(),
                                 std::chrono::seconds{10}))
        {
            std::wcerr << L"ERROR: The game process did not retain "
                       << Library.filename().wstring()
                       << L" after LoadLibraryW (truncated remote result " << Result
                       << L").\n";
            return false;
        }
        return true;
    }

    auto SetModsPathForChild(const fs::path& ModsPath) -> std::optional<std::wstring>
    {
        const auto Required = GetEnvironmentVariableW(L"UE4SS_MODS_PATHS", nullptr, 0);
        std::optional<std::wstring> Previous{};
        if (Required > 0)
        {
            std::vector<wchar_t> Buffer(Required);
            if (GetEnvironmentVariableW(L"UE4SS_MODS_PATHS", Buffer.data(), Required) > 0)
            {
                Previous = Buffer.data();
            }
        }
        // An explicit UE4SS_MODS_PATHS value is an override. Preserve it exactly so
        // MO2 or a controlled compatibility run cannot be shadowed by stale physical
        // mods. The physical game Mods directory remains the default when no override
        // was supplied.
        auto Value = Previous && !Previous->empty() ? *Previous : ModsPath.wstring();
        if (!SetEnvironmentVariableW(L"UE4SS_MODS_PATHS", Value.c_str()))
        {
            throw std::runtime_error("SetEnvironmentVariableW failed");
        }
        return Previous;
    }

    auto RestoreModsPath(const std::optional<std::wstring>& Previous) -> void
    {
        SetEnvironmentVariableW(L"UE4SS_MODS_PATHS",
                                Previous ? Previous->c_str() : nullptr);
    }

    struct FOptions
    {
        std::optional<fs::path> GameExecutable{};
        std::vector<std::wstring> GameArguments{};
        bool ValidateOnly{};
        bool Help{};
    };

    auto ParseOptions(const int ArgumentCount, wchar_t** Arguments) -> FOptions
    {
        FOptions Options{};
        bool Passthrough{};
        for (int Index = 1; Index < ArgumentCount; ++Index)
        {
            const std::wstring_view Argument{Arguments[Index]};
            if (!Passthrough && Argument == L"--")
            {
                Passthrough = true;
            }
            else if (!Passthrough && Argument == L"--game-exe")
            {
                if (++Index >= ArgumentCount)
                {
                    throw std::runtime_error("--game-exe requires a path");
                }
                Options.GameExecutable = fs::path{Arguments[Index]};
            }
            else if (!Passthrough && Argument == L"--validate-only")
            {
                Options.ValidateOnly = true;
            }
            else if (!Passthrough && (Argument == L"--help" || Argument == L"-h"))
            {
                Options.Help = true;
            }
            else
            {
                Options.GameArguments.emplace_back(Argument);
            }
        }
        return Options;
    }

    auto PrintUsage() -> void
    {
        std::wcout
                << L"UNBSELoader [--game-exe <path>] [--validate-only] [--] [game args...]\n"
                << L"Locates the current Steam game, validates the pinned UE4SS runtime, "
                   L"optionally checks .pak/.utoc/.ucas containers, and "
                   L"loads UE4SS after MO2's virtual filesystem is active.\n"
                << L"[UNBSE] EnablePluginVersionWarning in UE4SS-settings.ini controls "
                   L"the normal-launch plugin warning and defaults to 0 (off).\n"
                << L"[UNBSE] EnableAssetContainerWarning controls the normal-launch "
                   L"asset-container warning and defaults to 0 (off).\n"
                << L"[General] bUseUObjectArrayCache defaults to false; valid true/false "
                   L"developer choices are preserved.\n"
                << L"--validate-only reports plugin declarations and asset-container "
                   L"findings regardless of those warning toggles, without popups or "
                   L"launching the game.\n";
    }
} // namespace

auto wmain(const int ArgumentCount, wchar_t** Arguments) -> int
{
    try
    {
        const auto Options = ParseOptions(ArgumentCount, Arguments);
        if (Options.Help)
        {
            PrintUsage();
            return 0;
        }

        const auto SelfPath = GetSelfPath();
        const auto SelfDirectory = SelfPath.parent_path();
        const auto UE4SS = SelfDirectory / L"ue4ss" / L"UE4SS.dll";
        const auto Settings = SelfDirectory / L"ue4ss" / L"UE4SS-settings.ini";
        bool SettingsRepaired{};
        bool EnablePluginVersionWarning{};
        bool EnableAssetContainerWarning{};
        if (!ValidatePinnedFile(UE4SS, ExpectedUE4SSBytes, ExpectedUE4SSSha256,
                                L"Pinned UE4SS runtime") ||
            !EnsureCompatibleSettings(
                    Settings, Options.ValidateOnly, SettingsRepaired,
                    EnablePluginVersionWarning, EnableAssetContainerWarning))
        {
            return 2;
        }
        if (SettingsRepaired)
        {
            constexpr auto Message =
                    L"UNBSE updated UE4SS-settings.ini.\n\n"
                    L"Start UNBSELoader again from Vortex, Mod Organizer 2, or your "
                    L"normal launcher. The game was not started during this repair.";
            std::wcout << Message << L"\n";
            MessageBoxW(nullptr, Message, L"UNBSE Settings Updated - Launch Again",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            return 0;
        }

        const auto Game = FindGameExecutable(Options.GameExecutable, SelfDirectory);
        if (!Game)
        {
            std::wcerr << L"ERROR: Could not locate " << GameExecutableName
                       << L". Supply --game-exe <absolute-path>.\n";
            return 3;
        }
        const auto GameDirectory = Game->parent_path();
        const auto GameHash = Sha256File(*Game);
        const bool ExactGame = GameHash == ExpectedGameSha256;
        if (!ExactGame)
        {
            std::wcerr << L"WARNING: Game SHA-256 is not the verified Steam 1.512.105.0 "
                          L"build; continuing as an unverified attempt.\n";
        }

        std::error_code Error{};
        for (const auto LoaderName : ConflictingUE4SSLoaderNames)
        {
            Error.clear();
            const auto ConflictingLoader = GameDirectory / LoaderName;
            if (fs::is_regular_file(ConflictingLoader, Error))
            {
                std::wcerr
                        << L"ERROR: A conflicting UE4SS proxy loader is installed beside the game: "
                        << ConflictingLoader
                        << L"\nRemove it and any Mod Organizer 2 force-load entry. "
                           L"UNBSELoader loads its pinned UE4SS.dll only after the manager VFS "
                           L"is initialized.\n";
                return 4;
            }
        }

        std::wcout << L"UNBSE loader: " << SelfPath << L"\n"
                   << L"Supported runtime: " << SupportedRuntime << L" ("
                   << (ExactGame ? L"exact hash verified" : L"unverified attempt") << L")\n"
                   << L"Game: " << *Game << L"\n"
                   << L"UE4SS: " << UE4SS << L"\n";
        const auto AssetReview =
                Options.ValidateOnly || EnableAssetContainerWarning
                        ? RC::UNBSE::Preflight::ReviewAssetContainers(GameDirectory)
                        : RC::UNBSE::Preflight::FAssetContainerReview{};
        const auto PluginWarnings =
                Options.ValidateOnly || EnablePluginVersionWarning
                        ? ReviewPluginVersions(GameDirectory)
                        : std::vector<FPluginPreflightWarning>{};
        if (Options.ValidateOnly || EnableAssetContainerWarning)
        {
            std::wcout << L"Asset containers: " << AssetReview.TocContainers
                       << L" IoStore, " << AssetReview.PakContainers
                       << L" pak; serialization header verified "
                       << AssetReview.SerializationVersionsVerified
                       << L", envelope-only "
                       << AssetReview.SerializationVersionsUnverified
                       << L"; pak index verified " << AssetReview.PakIndexesVerified
                       << L", envelope-only " << AssetReview.PakIndexesUnverified
                       << L".\n";
        }
        else
        {
            std::wcout << L"Asset-container warning: disabled by [UNBSE] "
                          L"EnableAssetContainerWarning = 0.\n";
        }
        if (Options.ValidateOnly)
        {
            PrintPluginVersionWarnings(PluginWarnings);
            PrintAssetWarnings(AssetReview.Warnings);
            std::wcout << L"PASS: discovery and pinned-runtime validation succeeded; "
                       << PluginWarnings.size() << L" plugin warning(s) and "
                       << AssetReview.Warnings.size()
                       << L" asset-container warning(s) reported.\n";
            return 0;
        }
        if (EnablePluginVersionWarning)
        {
            const auto PendingWarnings =
                    PendingPluginWarnings(PluginWarnings, GameHash);
            PrintPluginVersionWarnings(PendingWarnings);
            if (!ConfirmPluginVersionWarnings(PendingWarnings))
            {
                std::wcerr
                        << L"CANCELLED: game launch stopped at the plugin-version warning.\n";
                return 7;
            }
            RememberPluginWarnings(PendingWarnings, GameHash);
        }
        if (EnableAssetContainerWarning)
        {
            const auto PendingAssets =
                    PendingAssetWarnings(AssetReview.Warnings, GameHash);
            PrintPendingAssetWarnings(PendingAssets);
            if (!ConfirmAssetWarnings(PendingAssets))
            {
                std::wcerr << L"CANCELLED: game launch stopped at the "
                              L"asset-container warning.\n";
                return 8;
            }
            RememberAssetWarnings(PendingAssets, GameHash);
        }

        auto CommandLine = BuildCommandLine(*Game, Options.GameArguments);
        std::vector<wchar_t> MutableCommandLine(CommandLine.begin(), CommandLine.end());
        MutableCommandLine.push_back(L'\0');

        const auto PreviousModsPath = SetModsPathForChild(
                GameDirectory / L"ue4ss" / L"Mods");
        STARTUPINFOW Startup{};
        Startup.cb = sizeof(Startup);
        PROCESS_INFORMATION ProcessInfo{};
        const auto Created = CreateProcessW(
                Game->c_str(), MutableCommandLine.data(), nullptr, nullptr, FALSE,
                CREATE_SUSPENDED, nullptr, GameDirectory.c_str(), &Startup, &ProcessInfo);
        const auto CreateError = GetLastError();
        RestoreModsPath(PreviousModsPath);
        if (!Created)
        {
            std::wcerr << L"ERROR: CreateProcessW failed: "
                       << FormatWindowsError(CreateError) << L"\n";
            return 5;
        }

        FHandle Process{ProcessInfo.hProcess};
        FHandle MainThread{ProcessInfo.hThread};
        const bool UnderMO2 = GetModuleHandleW(L"usvfs_x64.dll") != nullptr;
        bool InjectionSucceeded{};
        if (UnderMO2)
        {
            if (ResumeThread(MainThread.get()) == static_cast<DWORD>(-1))
            {
                std::wcerr << L"ERROR: Unable to start the MO2-injected game process.\n";
            }
            else if (!WaitForRemoteModule(Process.get(), ProcessInfo.dwProcessId,
                                          L"usvfs_x64.dll", std::chrono::seconds{10}))
            {
                std::wcerr << L"ERROR: MO2 did not initialize USVFS in the game process.\n";
            }
            else
            {
                // Module enumeration observes the image before its initialization routine has
                // necessarily installed every hook. A short bounded grace period keeps UE4SS's
                // first Mods scan behind that initialization without delaying normal launches.
                std::this_thread::sleep_for(std::chrono::milliseconds{250});
                InjectionSucceeded = InjectLibrary(
                        Process.get(), ProcessInfo.dwProcessId, UE4SS);
            }
        }
        else
        {
            if (ResumeThread(MainThread.get()) == static_cast<DWORD>(-1))
            {
                std::wcerr << L"ERROR: ResumeThread failed: "
                           << FormatWindowsError(GetLastError()) << L"\n";
            }
            else
            {
                InjectionSucceeded = InjectLibrary(
                        Process.get(), ProcessInfo.dwProcessId, UE4SS);
            }
        }

        if (!InjectionSucceeded)
        {
            TerminateProcess(Process.get(), 1);
            return 6;
        }
        std::wcout << L"PASS: pinned UE4SS loaded; the game is continuing.\n";
        return 0;
    }
    catch (const std::exception& Error)
    {
        std::cerr << "ERROR: " << Error.what() << "\n";
        return 1;
    }
}
