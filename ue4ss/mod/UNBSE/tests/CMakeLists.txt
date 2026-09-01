cmake_minimum_required(VERSION 3.22)

project(UNBSEScriptServiceRegistryTests LANGUAGES CXX)

enable_testing()

add_executable(UNBSEScriptServiceRegistryVmBindingTests
    ScriptServiceRegistryVmBindingTests.cpp
    ../src/ScriptServiceRegistry.cpp
)

target_include_directories(UNBSEScriptServiceRegistryVmBindingTests PRIVATE
    ../include
    ../../../../include
)
target_compile_features(UNBSEScriptServiceRegistryVmBindingTests PRIVATE cxx_std_23)
target_compile_definitions(UNBSEScriptServiceRegistryVmBindingTests PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

add_test(
    NAME UNBSEScriptServiceRegistryVmBindingTests
    COMMAND UNBSEScriptServiceRegistryVmBindingTests
)
