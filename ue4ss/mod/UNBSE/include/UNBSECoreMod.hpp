#pragma once

#include <memory>

#include <Mod/CppUserModBase.hpp>

namespace RC::UNBSE
{
    class FAddonRegistry;
    class FScriptServiceRegistry;

    class UNBSECoreMod final : public CppUserModBase
    {
      public:
        UNBSECoreMod();
        ~UNBSECoreMod() override;

        auto on_unreal_init() -> void override;
        auto on_lua_start(StringViewType ModName,
                          LuaMadeSimple::Lua& Lua,
                          LuaMadeSimple::Lua& MainLua,
                          LuaMadeSimple::Lua& AsyncLua,
                          LuaMadeSimple::Lua* HookLua) -> void override;
        auto on_lua_stop(StringViewType ModName,
                         LuaMadeSimple::Lua& Lua,
                         LuaMadeSimple::Lua& MainLua,
                         LuaMadeSimple::Lua& AsyncLua,
                         LuaMadeSimple::Lua* HookLua) -> void override;

      private:
        std::unique_ptr<FScriptServiceRegistry> m_script_service_registry{};
        std::unique_ptr<FAddonRegistry> m_addon_registry{};
        bool m_runtime_ready_dispatched{};
    };
} // namespace RC::UNBSE
