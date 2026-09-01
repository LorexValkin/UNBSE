#include <ScriptServiceRegistry.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{
    auto ExpectEqual(
            const std::uint32_t Actual,
            const std::uint32_t Expected,
            const char* Scenario) -> bool
    {
        if (Actual == Expected)
        {
            return true;
        }

        std::cerr << Scenario << ": expected " << Expected << ", got " << Actual << '\n';
        return false;
    }

    auto ExpectEqual(
            const std::size_t Actual,
            const std::size_t Expected,
            const char* Scenario) -> bool
    {
        if (Actual == Expected)
        {
            return true;
        }

        std::cerr << Scenario << ": expected " << Expected << ", got " << Actual << '\n';
        return false;
    }

    auto ExpectCapabilityEnabled(
            const std::uint32_t Result,
            const std::string_view ModName) -> bool
    {
        const auto Capability = Result == UNBSE_SCRIPT_RESULT_OK;
        if (Capability)
        {
            return true;
        }

        std::cerr << "observed load-order attachment for " << ModName
                  << ": expected capability:true,resultCode:0, got capability:false,resultCode:"
                  << Result << '\n';
        return false;
    }
} // namespace

auto main() -> int
{
    using namespace std::chrono_literals;
    using RC::UNBSE::FScriptServiceRegistry;

    static_assert(FScriptServiceRegistry::MaxVmBindings == 4096);

    constexpr std::uintptr_t FirstVmIdentity = 0x1000;
    constexpr std::uintptr_t ReplacementVmIdentity = 0x100000;
    constexpr std::uintptr_t OverflowVmIdentity = 0x200000;
    constexpr std::uint32_t AttachmentThreadId = 0x1234;
    constexpr std::uint32_t OtherThreadId = 0x5678;

    FScriptServiceRegistry Registry;
    bool Passed = true;

    constexpr std::array<std::string_view, 42> ObservedLuaLoadOrder{
            "BPML_GenericFunctions",
            "BPModLoaderMod",
            "AutoConfirmPrompts",
            "BeastMenu",
            "BowHeadshotDamage",
            "ClothingMenu",
            "CoolEnchantingMod",
            "DynamicBreathMultiplier",
            "EnhancedLightRadius",
            "FasterListScrolling",
            "HideArmor",
            "HideQuiver",
            "ImprovedAmbientOcclusion",
            "KwaNotificationsLua",
            "LumenRemastered",
            "MadCloak",
            "MadEXP",
            "MadExtender",
            "MadMCM",
            "MadSpellBind",
            "MadSpellDagon",
            "MadSpellPack",
            "MadSpellScaling",
            "MadTransform",
            "MadWeaponRescaler",
            "MeleeStagger",
            "NaturalBodyMorph",
            "NPCAppearanceManager",
            "OBRDremoraHorns",
            "OBRFirstPersonSkin",
            "RaceMenuUtilities",
            "ShadowsReworked",
            "SimplySwimUp",
            "SkipContinuePrompt",
            "SmithingMenu",
            "SpellBowStagger",
            "SpellSneak",
            "TesSyncMapInjector",
            "UNBSE",
            "UNBSEOBSE64Interop",
            "UpdateSourceFormForEnchant",
            "WmkTimeOnHUD",
    };
    constexpr std::size_t NaturalBodyMorphIndex = 26;
    static_assert(std::string_view{ObservedLuaLoadOrder[NaturalBodyMorphIndex]} ==
                  "NaturalBodyMorph");

    {
        FScriptServiceRegistry LoadOrderRegistry;
        constexpr std::uintptr_t LoadOrderFirstVmIdentity = 0x400000;

        for (std::size_t Index = 0; Index < ObservedLuaLoadOrder.size(); ++Index)
        {
            Passed &= ExpectCapabilityEnabled(
                    LoadOrderRegistry.AttachVm(
                            LoadOrderFirstVmIdentity + Index, AttachmentThreadId),
                    ObservedLuaLoadOrder[Index]);
        }
        Passed &= ExpectEqual(
                LoadOrderRegistry.VmCount(),
                ObservedLuaLoadOrder.size(),
                "attach every VM in the observed Lua load order");
    }

    for (std::size_t Index = 0; Index < 64; ++Index)
    {
        Passed &= ExpectEqual(
                Registry.AttachVm(FirstVmIdentity + Index, AttachmentThreadId),
                UNBSE_SCRIPT_RESULT_OK,
                "attach one of the first 64 unique VM identities");
    }
    Passed &= ExpectEqual(Registry.VmCount(), std::size_t{64}, "count the first 64 VM identities");

    Passed &= ExpectEqual(
            Registry.AttachVm(FirstVmIdentity, AttachmentThreadId),
            UNBSE_SCRIPT_RESULT_OK,
            "reattach the same VM identity on the same thread");
    Passed &= ExpectEqual(Registry.VmCount(), std::size_t{64}, "keep duplicate attachment idempotent");

    Passed &= ExpectEqual(
            Registry.AttachVm(FirstVmIdentity, OtherThreadId),
            UNBSE_SCRIPT_RESULT_VM_THREAD_MISMATCH,
            "reject duplicate VM attachment on a different thread");
    Passed &= ExpectEqual(
            Registry.Invoke(
                            FirstVmIdentity,
                            OtherThreadId,
                            "missing",
                            "missing",
                            nullptr,
                            0,
                            1ms)
                    .Code,
            UNBSE_SCRIPT_RESULT_VM_THREAD_MISMATCH,
            "preserve invocation thread-mismatch behavior");
    Passed &= ExpectEqual(Registry.VmCount(), std::size_t{64}, "keep mismatch attempts out of the VM count");

    for (std::size_t Index = 64; Index < FScriptServiceRegistry::MaxVmBindings; ++Index)
    {
        Passed &= ExpectEqual(
                Registry.AttachVm(FirstVmIdentity + Index, AttachmentThreadId),
                UNBSE_SCRIPT_RESULT_OK,
                "attach through the configured VM capacity");
    }
    Passed &= ExpectEqual(
            Registry.VmCount(),
            FScriptServiceRegistry::MaxVmBindings,
            "count the configured VM capacity");
    Passed &= ExpectEqual(
            Registry.AttachVm(OverflowVmIdentity, AttachmentThreadId),
            UNBSE_SCRIPT_RESULT_REGISTRY_FULL,
            "reject the first VM beyond the configured capacity");

    Passed &= ExpectEqual(
            Registry.DetachVm(FirstVmIdentity, 0ms),
            UNBSE_SCRIPT_RESULT_OK,
            "detach an idle VM");
    Passed &= ExpectEqual(
            Registry.VmCount(),
            FScriptServiceRegistry::MaxVmBindings - 1,
            "decrement the VM count after detach");
    Passed &= ExpectEqual(
            Registry.AttachVm(ReplacementVmIdentity, AttachmentThreadId),
            UNBSE_SCRIPT_RESULT_OK,
            "reuse a detached VM slot");
    Passed &= ExpectEqual(
            Registry.VmCount(),
            FScriptServiceRegistry::MaxVmBindings,
            "restore the VM count after slot reuse");
    Passed &= ExpectEqual(
            Registry.AttachVm(OverflowVmIdentity, AttachmentThreadId),
            UNBSE_SCRIPT_RESULT_REGISTRY_FULL,
            "preserve the full-capacity boundary after slot reuse");

    return Passed ? 0 : 1;
}
