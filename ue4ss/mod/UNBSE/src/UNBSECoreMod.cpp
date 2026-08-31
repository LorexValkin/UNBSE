#include <UNBSECoreMod.hpp>

#include <chrono>
#include <string>

#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>

#include <AddonRegistry.hpp>
#include <CoreLuaBinding.hpp>
#include <ScriptServiceRegistry.hpp>

namespace RC::UNBSE
{
    namespace
    {
        constexpr auto FoundationId =
                STR("ue4ss-3.0.1-beta0-68dd45cb-unbse-patchset-v1-mod-0.13.1-rc.1");
        constexpr auto FoundationIdUtf8 =
                "ue4ss-3.0.1-beta0-68dd45cb-unbse-patchset-v1-mod-0.13.1-rc.1";

        auto LifecycleResultName(const std::uint32_t Result) -> const wchar_t*
        {
            return Result == UNBSE_MESSAGING_RESULT_OK
                           ? STR("dispatched")
                           : Result == UNBSE_MESSAGING_RESULT_NO_LISTENERS
                                     ? STR("no-listeners")
                                     : STR("dispatch-failed");
        }
    } // namespace

    UNBSECoreMod::UNBSECoreMod()
    {
        ModName = STR("UNBSE");
        ModVersion = STR("0.13.1-rc.1");
        ModDescription = STR("Unblivion Script Extender runtime capability injector");
        ModAuthors = STR("Unblivion Project");
        ModIntendedSDKVersion = STR("3.0.1");

        m_script_service_registry = std::make_unique<FScriptServiceRegistry>();
        if (!PublishScriptServiceRegistry(m_script_service_registry.get()))
        {
            Output::send<LogLevel::Error>(
                    STR("[UNBSE.Core] {\"schema\":\"UNBSE.CoreStartup\",\"schemaVersion\":1,"
                        "\"status\":\"script-registry-publication-failed\"}\n"));
            m_script_service_registry.reset();
        }
        else
        {
            const auto Description = RegisterCoreRuntimeDescription(*m_script_service_registry);
            if (Description != UNBSE_SCRIPT_RESULT_OK)
            {
                Output::send<LogLevel::Error>(
                        STR("[UNBSE.Core] {{\"schema\":\"UNBSE.CoreStartup\",\"schemaVersion\":1,"
                            "\"status\":\"runtime-description-registration-failed\","
                            "\"resultCode\":{}}}\n"),
                        Description);
                UnpublishScriptServiceRegistry(m_script_service_registry.get());
                m_script_service_registry.reset();
            }
            else
            {
                m_addon_registry = std::make_unique<FAddonRegistry>(
                        *m_script_service_registry,
                        [](const std::string& Event) {
                            Output::send(STR("[UNBSE.Addon] {}\n"), ensure_str(Event));
                        },
                        FAddonRuntimeIdentity{"0.13.1-rc.1", FoundationIdUtf8});
                if (!PublishAddonRegistry(m_addon_registry.get()))
                {
                    Output::send<LogLevel::Error>(
                            STR("[UNBSE.Core] {\"schema\":\"UNBSE.CoreStartup\","
                                "\"schemaVersion\":1,\"status\":\"addon-host-publication-failed\"}\n"));
                    m_addon_registry.reset();
                    UnpublishScriptServiceRegistry(m_script_service_registry.get());
                    m_script_service_registry.reset();
                }
            }
        }

        Output::send(
                STR("[UNBSE.Core] {{\"schema\":\"UNBSE.CoreStartup\",\"schemaVersion\":1,"
                    "\"foundationId\":\"{}\",\"version\":\"0.13.1-rc.1\","
                    "\"role\":\"runtime-capability-injector\","
                    "\"compatibilityPolicy\":\"report-and-attempt\","
                    "\"bundledProbes\":false,\"bundledMcp\":false,\"status\":\"constructed\"}}\n"),
                FoundationId);
    }

    UNBSECoreMod::~UNBSECoreMod()
    {
        if (m_addon_registry)
        {
            const auto Lifecycle = m_addon_registry->DispatchCore(
                    UNBSE_CORE_MESSAGE_RUNTIME_STOPPING, nullptr, 0);
            Output::send(
                    STR("[UNBSE.Lifecycle] {{\"schema\":\"UNBSE.Lifecycle\",\"schemaVersion\":1,"
                        "\"event\":\"runtime-stopping\",\"status\":\"{}\","
                        "\"resultCode\":{}}}\n"),
                    LifecycleResultName(Lifecycle), Lifecycle);
            UnpublishAddonRegistry(m_addon_registry.get());
            const auto Shutdown = m_addon_registry->BeginShutdown(
                    std::chrono::milliseconds{UNBSE_SCRIPT_DEFAULT_DEADLINE_MS});
            if (Shutdown != UNBSE_ADDON_RESULT_OK)
            {
                Output::send<LogLevel::Error>(
                        STR("[UNBSE.Core] {{\"schema\":\"UNBSE.CoreShutdown\",\"schemaVersion\":1,"
                            "\"status\":\"addon-shutdown-incomplete\",\"resultCode\":{}}}\n"),
                        Shutdown);
            }
            m_addon_registry.reset();
        }
        if (m_script_service_registry)
        {
            UnpublishScriptServiceRegistry(m_script_service_registry.get());
            const auto Shutdown = m_script_service_registry->BeginShutdown(
                    std::chrono::milliseconds{UNBSE_SCRIPT_DEFAULT_DEADLINE_MS});
            if (Shutdown != UNBSE_SCRIPT_RESULT_OK)
            {
                Output::send<LogLevel::Error>(
                        STR("[UNBSE.Core] {{\"schema\":\"UNBSE.CoreShutdown\",\"schemaVersion\":1,"
                            "\"status\":\"script-shutdown-incomplete\",\"resultCode\":{}}}\n"),
                        Shutdown);
            }
            m_script_service_registry.reset();
        }
    }

    auto UNBSECoreMod::on_unreal_init() -> void
    {
        if (!m_addon_registry || m_runtime_ready_dispatched)
        {
            return;
        }
        m_runtime_ready_dispatched = true;
        const auto Result = m_addon_registry->DispatchCore(
                UNBSE_CORE_MESSAGE_RUNTIME_READY, nullptr, 0);
        Output::send(
                STR("[UNBSE.Lifecycle] {{\"schema\":\"UNBSE.Lifecycle\",\"schemaVersion\":1,"
                    "\"event\":\"runtime-ready\",\"windowsThreadId\":{},\"status\":\"{}\","
                    "\"resultCode\":{}}}\n"),
                GetCurrentThreadId(), LifecycleResultName(Result), Result);
    }

    auto UNBSECoreMod::on_lua_start(StringViewType ModName,
                                    LuaMadeSimple::Lua&,
                                    LuaMadeSimple::Lua& MainLua,
                                    LuaMadeSimple::Lua&,
                                    LuaMadeSimple::Lua*) -> void
    {
        if (!m_script_service_registry)
        {
            return;
        }
        const auto Result = AttachCoreLuaVm(*m_script_service_registry, MainLua);
        Output::send(
                STR("[UNBSE.ScriptService] {{\"schema\":\"UNBSE.ScriptService\","
                    "\"schemaVersion\":1,\"event\":\"ue4ss-lua-start\",\"modName\":\"{}\","
                    "\"backend\":\"ue4ss_lua_v1\",\"windowsThreadId\":{},"
                    "\"capability\":{},\"resultCode\":{}}}\n"),
                ModName, GetCurrentThreadId(), Result == UNBSE_SCRIPT_RESULT_OK, Result);
        if (Result == UNBSE_SCRIPT_RESULT_OK && m_addon_registry)
        {
            const auto Lifecycle = m_addon_registry->DispatchCore(
                    UNBSE_CORE_MESSAGE_LUA_READY, nullptr, 0);
            Output::send(
                    STR("[UNBSE.Lifecycle] {{\"schema\":\"UNBSE.Lifecycle\","
                        "\"schemaVersion\":1,\"event\":\"lua-ready\",\"status\":\"{}\","
                        "\"resultCode\":{}}}\n"),
                    LifecycleResultName(Lifecycle), Lifecycle);
        }
    }

    auto UNBSECoreMod::on_lua_stop(StringViewType ModName,
                                   LuaMadeSimple::Lua&,
                                   LuaMadeSimple::Lua& MainLua,
                                   LuaMadeSimple::Lua&,
                                   LuaMadeSimple::Lua*) -> void
    {
        if (!m_script_service_registry)
        {
            return;
        }
        const auto Result = DetachCoreLuaVm(*m_script_service_registry, MainLua);
        Output::send(
                STR("[UNBSE.ScriptService] {{\"schema\":\"UNBSE.ScriptService\","
                    "\"schemaVersion\":1,\"event\":\"ue4ss-lua-stop\",\"modName\":\"{}\","
                    "\"backend\":\"ue4ss_lua_v1\",\"windowsThreadId\":{},"
                    "\"capability\":false,\"resultCode\":{}}}\n"),
                ModName, GetCurrentThreadId(), Result);
    }
} // namespace RC::UNBSE
