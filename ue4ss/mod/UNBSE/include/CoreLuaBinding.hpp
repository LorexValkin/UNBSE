#pragma once

#include <cstdint>

namespace RC::LuaMadeSimple
{
    class Lua;
}

namespace RC::UNBSE
{
    class FScriptServiceRegistry;

    auto RegisterCoreRuntimeDescription(FScriptServiceRegistry& Registry) -> std::uint32_t;
    auto AttachCoreLuaVm(FScriptServiceRegistry& Registry, LuaMadeSimple::Lua& MainLua)
            -> std::uint32_t;
    auto DetachCoreLuaVm(FScriptServiceRegistry& Registry, LuaMadeSimple::Lua& MainLua)
            -> std::uint32_t;
} // namespace RC::UNBSE
