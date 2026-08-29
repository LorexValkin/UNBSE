#pragma once

#include <OBSE64PluginABI.hpp>
#include <OBSE64PluginScanner.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RC::UNBSE::OBSE64
{
    struct FPluginManagerConfig
    {
        std::uint32_t RuntimeVersion{};
        std::uint32_t EmulatedScriptExtenderVersion{};
        std::uint32_t ReleaseIndex{};
        std::string SaveFolderName{"Oblivion Remastered"};
        bool AddressLibraryAvailable{};
        bool PreloadTimingAvailable{};
    };

    struct FPluginLoadSummary
    {
        std::size_t Scanned{};
        std::size_t Accepted{};
        std::size_t Loaded{};
        std::size_t Rejected{};
        std::size_t LoadFailed{};
        std::size_t UnverifiedAttempts{};
    };

    using FPluginManagerEventSink = std::function<void(const std::string&)>;

    class FPluginManager final
    {
      public:
        explicit FPluginManager(FPluginManagerConfig Config,
                                FPluginManagerEventSink EventSink = {});
        ~FPluginManager();
        FPluginManager(const FPluginManager&) = delete;
        FPluginManager& operator=(const FPluginManager&) = delete;

        auto LoadPaths(const std::vector<std::filesystem::path>& Paths) -> FPluginLoadSummary;
        auto DispatchPostLoad() -> bool;
        auto DispatchDataLoaded() -> bool;
        static auto IsDataLoadedHookAnchor(std::uintptr_t CallSite,
                                           std::uintptr_t ExpectedTarget) -> bool;
        auto InstallDataLoadedHook(std::uintptr_t CallSite,
                                   std::uintptr_t ExpectedTarget) -> bool;
        auto RemoveDataLoadedHook() -> bool;
        auto Shutdown() -> void;
        auto LoadedCount() const -> std::size_t;
        auto GetInfo(const char* Name) const -> const FPluginInfo*;
        auto TrampolineAvailable() const -> bool;
        auto DataLoadedHookInstalled() const -> bool;
        auto DataLoadedDispatched() const -> bool;

      private:
        struct FImpl;
        std::unique_ptr<FImpl> m_impl;
    };
} // namespace RC::UNBSE::OBSE64
