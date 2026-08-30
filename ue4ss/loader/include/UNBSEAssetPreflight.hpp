#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace RC::UNBSE::Preflight
{
    struct FAssetContainerWarning
    {
        std::wstring Name{};
        std::filesystem::path Path{};
        std::wstring Reason{};
        std::wstring Evidence{};
        std::wstring FingerprintMaterial{};
        bool FingerprintPrimaryFile{};
    };

    struct FAssetContainerReview
    {
        std::vector<FAssetContainerWarning> Warnings{};
        std::size_t TocContainers{};
        std::size_t PakContainers{};
        std::size_t SerializationVersionsVerified{};
        std::size_t SerializationVersionsUnverified{};
        std::size_t PakIndexesVerified{};
        std::size_t PakIndexesUnverified{};
    };

    auto ReviewAssetContainers(const std::filesystem::path& GameBinaryDirectory)
            -> FAssetContainerReview;
} // namespace RC::UNBSE::Preflight
