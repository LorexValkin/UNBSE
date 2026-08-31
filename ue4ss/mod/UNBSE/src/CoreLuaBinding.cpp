#include <CoreLuaBinding.hpp>

#include <ScriptServiceRegistry.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>

namespace RC::UNBSE
{
    namespace
    {
        constexpr auto LuaNamespace = "UNBSE";
        constexpr auto LuaBackend = "ue4ss_lua_v1";
        constexpr auto NamespaceRegistryField = "UNBSE.CoreLua.Owner/1";

        auto GetLuaVmRootState(lua_State* State) -> lua_State*
        {
            lua_rawgeti(State, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
            auto* MainState = lua_tothread(State, -1);
            lua_pop(State, 1);
            return MainState;
        }

        auto PushFailure(lua_State* State, const std::uint32_t Code, const std::string& Diagnostic)
                -> int
        {
            lua_pushnil(State);
            lua_pushinteger(State, static_cast<lua_Integer>(Code));
            lua_pushlstring(State, Diagnostic.data(), Diagnostic.size());
            return 3;
        }

        auto PushValue(lua_State* State, const UNBSEScriptValueV1& Value) -> void
        {
            switch (Value.type)
            {
            case UNBSE_SCRIPT_VALUE_NONE: lua_pushnil(State); return;
            case UNBSE_SCRIPT_VALUE_BOOL:
                lua_pushboolean(State, Value.integerValue != 0);
                return;
            case UNBSE_SCRIPT_VALUE_INT64:
                lua_pushinteger(State, static_cast<lua_Integer>(Value.integerValue));
                return;
            case UNBSE_SCRIPT_VALUE_FLOAT64:
                lua_pushnumber(State, static_cast<lua_Number>(Value.numberValue));
                return;
            case UNBSE_SCRIPT_VALUE_UTF8: lua_pushstring(State, Value.utf8Value); return;
            default: lua_pushnil(State); return;
            }
        }

        auto ReadToken(lua_State* State, const int Index, std::string* Value) -> bool
        {
            if (lua_type(State, Index) != LUA_TSTRING)
            {
                return false;
            }
            std::size_t Length{};
            const auto Bytes = lua_tolstring(State, Index, &Length);
            if (!Bytes || Length == 0 || Length >= UNBSE_SCRIPT_TOKEN_BYTES ||
                std::memchr(Bytes, '\0', Length) != nullptr)
            {
                return false;
            }
            Value->assign(Bytes, Length);
            return true;
        }

        auto ReadValue(lua_State* State, const int Index, UNBSEScriptValueV1* Value) -> bool
        {
            *Value = {};
            Value->structSize = sizeof(UNBSEScriptValueV1);
            switch (lua_type(State, Index))
            {
            case LUA_TBOOLEAN:
                Value->type = UNBSE_SCRIPT_VALUE_BOOL;
                Value->integerValue = lua_toboolean(State, Index) ? 1 : 0;
                return true;
            case LUA_TNUMBER:
                if (lua_isinteger(State, Index))
                {
                    Value->type = UNBSE_SCRIPT_VALUE_INT64;
                    Value->integerValue = static_cast<std::int64_t>(lua_tointeger(State, Index));
                }
                else
                {
                    Value->type = UNBSE_SCRIPT_VALUE_FLOAT64;
                    Value->numberValue = static_cast<double>(lua_tonumber(State, Index));
                }
                return true;
            case LUA_TSTRING:
            {
                std::size_t Length{};
                const auto Bytes = lua_tolstring(State, Index, &Length);
                if (!Bytes || Length >= UNBSE_SCRIPT_UTF8_BYTES ||
                    std::memchr(Bytes, '\0', Length) != nullptr)
                {
                    return false;
                }
                Value->type = UNBSE_SCRIPT_VALUE_UTF8;
                std::memcpy(Value->utf8Value, Bytes, Length);
                Value->utf8Value[Length] = '\0';
                return true;
            }
            default: return false;
            }
        }

        auto LuaInvoke(lua_State* State) -> int
        {
            const auto Registry = GetPublishedScriptServiceRegistry();
            if (!Registry)
            {
                return PushFailure(State, UNBSE_SCRIPT_RESULT_VM_UNAVAILABLE,
                                   "UNBSE script registry is unavailable");
            }
            const auto Count = lua_gettop(State);
            if (Count < 2 || Count > static_cast<int>(2 + UNBSE_SCRIPT_MAX_ARGUMENTS))
            {
                return PushFailure(State, UNBSE_SCRIPT_RESULT_ARGUMENT_MISMATCH,
                                   "Invoke requires namespace, function, and at most eight values");
            }
            std::string NamespaceToken{};
            std::string FunctionToken{};
            if (!ReadToken(State, 1, &NamespaceToken) || !ReadToken(State, 2, &FunctionToken))
            {
                return PushFailure(State, UNBSE_SCRIPT_RESULT_ARGUMENT_MISMATCH,
                                   "namespace and function must be bounded UTF-8 tokens");
            }
            std::array<UNBSEScriptValueV1, UNBSE_SCRIPT_MAX_ARGUMENTS> Arguments{};
            const auto ArgumentCount = static_cast<std::uint32_t>(Count - 2);
            for (std::uint32_t Index = 0; Index < ArgumentCount; ++Index)
            {
                if (!ReadValue(State, static_cast<int>(Index + 3), &Arguments[Index]))
                {
                    return PushFailure(State, UNBSE_SCRIPT_RESULT_ARGUMENT_MISMATCH,
                                       "argument type or value is outside the fixed ABI");
                }
            }

            const auto Result = Registry->Invoke(
                    reinterpret_cast<std::uintptr_t>(GetLuaVmRootState(State)),
                    GetCurrentThreadId(), NamespaceToken, FunctionToken,
                    ArgumentCount == 0 ? nullptr : Arguments.data(), ArgumentCount,
                    std::chrono::milliseconds{UNBSE_SCRIPT_DEFAULT_DEADLINE_MS});
            Output::send(
                    STR("[UNBSE.ScriptInvocation] {{\"schema\":\"UNBSE.ScriptInvocation\","
                        "\"schemaVersion\":1,\"backend\":\"ue4ss_lua_v1\","
                        "\"namespace\":\"{}\",\"function\":\"{}\",\"sequence\":{},"
                        "\"windowsThreadId\":{},\"elapsedMs\":{},\"resultCode\":{}}}\n"),
                    ensure_str(NamespaceToken), ensure_str(FunctionToken), Result.Sequence,
                    GetCurrentThreadId(), Result.ElapsedMilliseconds, Result.Code);
            if (Result.Code != UNBSE_SCRIPT_RESULT_OK)
            {
                return PushFailure(State, Result.Code, Result.Diagnostic);
            }
            PushValue(State, Result.Value);
            lua_pushinteger(State, UNBSE_SCRIPT_RESULT_OK);
            return 2;
        }

        auto LuaDescribeService(lua_State* State) -> int
        {
            const auto Registry = GetPublishedScriptServiceRegistry();
            const auto Capabilities = Registry ? Registry->Capabilities() : 0;
            lua_createtable(State, 0, 9);
            lua_pushinteger(State, UNBSE_SCRIPT_SERVICE_ABI_VERSION);
            lua_setfield(State, -2, "apiVersion");
            lua_pushinteger(State, static_cast<lua_Integer>(Capabilities));
            lua_setfield(State, -2, "capabilities");
            lua_pushstring(State, LuaBackend);
            lua_setfield(State, -2, "backend");
            lua_pushboolean(State, (Capabilities & UNBSE_SCRIPT_CAPABILITY_UE4SS_LUA_VM) != 0);
            lua_setfield(State, -2, "ue4ssLuaVm");
            lua_pushboolean(State, false);
            lua_setfield(State, -2, "gameScriptVm");
            lua_pushboolean(State, true);
            lua_setfield(State, -2, "externalFunctionRegistration");
            lua_pushboolean(State, true);
            lua_setfield(State, -2, "readFunctionsSupported");
            lua_pushboolean(State, true);
            lua_setfield(State, -2, "writeFunctionsSupported");
            lua_pushstring(State, "report-and-attempt");
            lua_setfield(State, -2, "compatibilityPolicy");
            return 1;
        }

        void UNBSE_SCRIPT_CALL CoreDescribeRuntime(const UNBSEScriptInvocationV1* Invocation,
                                                   UNBSEScriptResultV1* Result,
                                                   void*)
        {
            Result->code = UNBSE_SCRIPT_RESULT_OK;
            Result->value.structSize = sizeof(UNBSEScriptValueV1);
            Result->value.type = UNBSE_SCRIPT_VALUE_UTF8;
            const auto Written = std::snprintf(
                    Result->value.utf8Value, sizeof(Result->value.utf8Value),
                    "{\"schema\":\"UNBSE.RuntimeDescription\",\"schemaVersion\":1,"
                    "\"version\":\"0.13.5\",\"role\":\"runtime-capability-injector\","
                    "\"compatibilityPolicy\":\"report-and-attempt\",\"sequence\":%llu,"
                    "\"windowsThreadId\":%u,\"bundledProbes\":false,\"bundledMcp\":false,"
                    "\"bundledGameFeatureAddons\":false,\"baseObse64Interop\":true}",
                    static_cast<unsigned long long>(Invocation->sequence),
                    Invocation->windowsThreadId);
            if (Written < 0 || static_cast<std::size_t>(Written) >=
                                       sizeof(Result->value.utf8Value))
            {
                Result->code = UNBSE_SCRIPT_RESULT_CALLBACK_FAILED;
                Result->value = {};
                Result->value.structSize = sizeof(UNBSEScriptValueV1);
                Result->value.type = UNBSE_SCRIPT_VALUE_NONE;
            }
        }

        auto OwnsLuaNamespace(lua_State* State) -> bool
        {
            if (lua_getglobal(State, LuaNamespace) != LUA_TTABLE)
            {
                lua_pop(State, 1);
                return false;
            }
            lua_getfield(State, LUA_REGISTRYINDEX, NamespaceRegistryField);
            const auto Matches = lua_rawequal(State, -1, -2) != 0;
            lua_pop(State, 2);
            return Matches;
        }

        auto ClearLuaNamespace(lua_State* State) -> void
        {
            if (OwnsLuaNamespace(State))
            {
                lua_pushnil(State);
                lua_setglobal(State, LuaNamespace);
            }
            lua_pushnil(State);
            lua_setfield(State, LUA_REGISTRYINDEX, NamespaceRegistryField);
        }
    } // namespace

    auto RegisterCoreRuntimeDescription(FScriptServiceRegistry& Registry) -> std::uint32_t
    {
        UNBSEScriptFunctionDeclarationV1 Declaration{};
        Declaration.structSize = sizeof(Declaration);
        Declaration.apiVersion = UNBSE_SCRIPT_SERVICE_ABI_VERSION;
        Declaration.flags = UNBSE_SCRIPT_FUNCTION_READ_ONLY;
        Declaration.resultType = UNBSE_SCRIPT_VALUE_UTF8;
        std::memcpy(Declaration.namespaceToken, "core", sizeof("core"));
        std::memcpy(Declaration.functionToken, "describe_runtime", sizeof("describe_runtime"));
        Declaration.callback = &CoreDescribeRuntime;
        return Registry.Register(FScriptServiceRegistry::CoreOwnerHandle, &Declaration);
    }

    auto AttachCoreLuaVm(FScriptServiceRegistry& Registry, LuaMadeSimple::Lua& MainLua)
            -> std::uint32_t
    {
        auto* State = MainLua.get_lua_state();
        if (!State)
        {
            return UNBSE_SCRIPT_RESULT_VM_UNAVAILABLE;
        }
        if (lua_getglobal(State, LuaNamespace) != LUA_TNIL)
        {
            lua_pop(State, 1);
            return UNBSE_SCRIPT_RESULT_NAMESPACE_COLLISION;
        }
        lua_pop(State, 1);

        lua_createtable(State, 0, 4);
        lua_pushinteger(State, UNBSE_SCRIPT_SERVICE_ABI_VERSION);
        lua_setfield(State, -2, "ApiVersion");
        lua_pushstring(State, LuaBackend);
        lua_setfield(State, -2, "Backend");
        lua_pushcfunction(State, &LuaInvoke);
        lua_setfield(State, -2, "Invoke");
        lua_pushcfunction(State, &LuaDescribeService);
        lua_setfield(State, -2, "DescribeService");
        lua_pushvalue(State, -1);
        lua_setfield(State, LUA_REGISTRYINDEX, NamespaceRegistryField);
        lua_setglobal(State, LuaNamespace);

        const auto Attached = Registry.AttachVm(
                reinterpret_cast<std::uintptr_t>(GetLuaVmRootState(State)), GetCurrentThreadId());
        if (Attached != UNBSE_SCRIPT_RESULT_OK)
        {
            ClearLuaNamespace(State);
        }
        return Attached;
    }

    auto DetachCoreLuaVm(FScriptServiceRegistry& Registry, LuaMadeSimple::Lua& MainLua)
            -> std::uint32_t
    {
        auto* State = MainLua.get_lua_state();
        if (!State)
        {
            return UNBSE_SCRIPT_RESULT_VM_UNAVAILABLE;
        }
        const auto Detached = Registry.DetachVm(
                reinterpret_cast<std::uintptr_t>(GetLuaVmRootState(State)),
                std::chrono::milliseconds{UNBSE_SCRIPT_DEFAULT_DEADLINE_MS});
        if (Detached == UNBSE_SCRIPT_RESULT_OK)
        {
            ClearLuaNamespace(State);
        }
        return Detached;
    }
} // namespace RC::UNBSE
