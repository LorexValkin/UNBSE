cmake_minimum_required(VERSION 3.22)

set(TARGET UNBSEOBSE64Interop)
project(${TARGET} LANGUAGES CXX)

add_library(${TARGET} SHARED
    src/OBSE64PluginManager.cpp
    src/OBSE64PluginScanner.cpp
    src/dllmain.cpp
)

target_include_directories(${TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(${TARGET} PRIVATE cxx_std_23)
target_compile_definitions(${TARGET} PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
)
target_link_libraries(${TARGET} PUBLIC UE4SS)
target_link_libraries(${TARGET} PRIVATE psapi version)

if (MSVC)
    target_compile_options(${TARGET} PRIVATE "/Brepro")
    target_link_options(${TARGET} PRIVATE "/Brepro" "/PDBALTPATH:%_PDB%")
endif()

set_target_properties(${TARGET} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "main"
    FOLDER "mods/UNBSEOBSE64Interop"
)
