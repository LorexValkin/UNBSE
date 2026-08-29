#include <UNBSECoreMod.hpp>

extern "C"
{
    __declspec(dllexport) RC::CppUserModBase* start_mod()
    {
        return new RC::UNBSE::UNBSECoreMod();
    }

    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* Mod)
    {
        delete Mod;
    }
}
