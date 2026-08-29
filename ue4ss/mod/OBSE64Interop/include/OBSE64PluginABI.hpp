#pragma once

// Clean-room compatible declarations for the public OBSE64 plugin ABI v1.
// Layout reference: ianpatt/obse64 obse64/PluginAPI.h at commit
// 09bdc6155032c19045feba876886465de0c71743. That public header is MIT
// licensed; no non-permissively licensed OBSE64 implementation is included.
#include <cstddef>
#include <cstdint>

namespace RC::UNBSE::OBSE64
{
    using PluginHandle = std::uint32_t;
    constexpr PluginHandle InvalidPluginHandle = 0xFFFFFFFFu;
    constexpr PluginHandle CorePluginHandle = 0;

    enum EInterfaceId : std::uint32_t
    {
        InterfaceInvalid = 0,
        InterfaceMessaging = 1,
        InterfaceTrampoline = 2
    };

    struct FPluginInfo
    {
        std::uint32_t InfoVersion{};
        const char* Name{};
        std::uint32_t Version{};
    };

    struct FInterface
    {
        std::uint32_t ScriptExtenderVersion{};
        std::uint32_t RuntimeVersion{};
        std::uint32_t EditorVersion{};
        std::uint32_t IsEditor{};
        std::uint32_t InterfaceVersion{};
        void* (*QueryInterface)(std::uint32_t Id){};
        PluginHandle (*GetPluginHandle)(){};
        std::uint32_t (*GetReleaseIndex)(){};
        const FPluginInfo* (*GetPluginInfo)(const char* Name){};
        const char* (*GetSaveFolderName)(){};
    };

    struct FMessagingInterface
    {
        struct FMessage
        {
            const char* Sender{};
            std::uint32_t Type{};
            std::uint32_t DataLength{};
            void* Data{};
        };

        using FEventCallback = void (*)(FMessage* Message);

        std::uint32_t InterfaceVersion{};
        bool (*RegisterListener)(PluginHandle Listener,
                                 const char* Sender,
                                 FEventCallback Handler){};
        bool (*Dispatch)(PluginHandle Sender,
                         std::uint32_t MessageType,
                         void* Data,
                         std::uint32_t DataLength,
                         const char* Receiver){};
    };

    struct FTrampolineInterface
    {
        std::uint32_t InterfaceVersion{};
        void* (*AllocateFromBranchPool)(PluginHandle Plugin, std::size_t Size){};
        void* (*AllocateFromLocalPool)(PluginHandle Plugin, std::size_t Size){};
    };

    using FPluginLoad = bool (*)(const FInterface* Interface);

    constexpr std::uint32_t PluginInfoVersion = 1;
    constexpr std::uint32_t InterfaceVersion = 1;
    constexpr std::uint32_t MessagingInterfaceVersion = 1;
    constexpr std::uint32_t TrampolineInterfaceVersion = 1;
    constexpr std::uint32_t MessagePostLoad = 0;
    constexpr std::uint32_t MessagePostPostLoad = 1;
    constexpr std::uint32_t MessageDataLoaded = 2;

    static_assert(sizeof(FPluginInfo) == 24, "OBSE64 PluginInfo ABI size drift");
    static_assert(sizeof(FInterface) == 64, "OBSE64 base interface ABI size drift");
    static_assert(sizeof(FMessagingInterface::FMessage) == 24,
                  "OBSE64 message ABI size drift");
    static_assert(sizeof(FMessagingInterface) == 24,
                  "OBSE64 messaging interface ABI size drift");
    static_assert(sizeof(FTrampolineInterface) == 24,
                  "OBSE64 trampoline interface ABI size drift");
} // namespace RC::UNBSE::OBSE64
